#include "libretro.h"

#include <ymir/ymir.hpp>

#include <ymir/db/ipl_db.hpp>
#include <ymir/media/loader/loader.hpp>
#include <ymir/state/state.hpp>
#include <ymir/sys/saturn.hpp>

#include <ymir/hw/smpc/peripheral/peripheral_port.hpp>
#include <ymir/hw/vdp/vdp_defs.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>
#include <cstdio>
#include <type_traits>

#if defined(_WIN32)
    #include <windows.h>
    #include <eh.h>
#endif

namespace {

using ymir::peripheral::Button;
using ymir::peripheral::PeripheralReport;
using ymir::peripheral::PeripheralType;

struct InputContext {
    unsigned port = 0;
};

struct LibretroContext {
    retro_environment_t env_cb = nullptr;
    retro_video_refresh_t video_cb = nullptr;
    retro_audio_sample_t audio_cb = nullptr;
    retro_audio_sample_batch_t audio_batch_cb = nullptr;
    retro_input_poll_t input_poll_cb = nullptr;
    retro_input_state_t input_state_cb = nullptr;
    retro_log_printf_t log_cb = nullptr;

    std::unique_ptr<ymir::Saturn> saturn;

    std::filesystem::path system_dir;
    std::filesystem::path save_dir;
    std::filesystem::path backup_ram_path;
    std::filesystem::path smpc_persist_path;

    std::vector<uint32_t> framebuffer_xbgr;
    std::vector<uint32_t> framebuffer_xrgb;
    unsigned fb_width = 320;
    unsigned fb_height = 224;
    bool frame_ready = false;

    std::vector<int16_t> audio_buffer;

    InputContext input_contexts[2] = {{0u}, {1u}};
};

LibretroContext g_ctx{};

constexpr unsigned kAudioSampleRate = 44100;

std::mutex g_logMtx;

constexpr size_t kRollbackStateSize = sizeof(ymir::state::State);
static_assert(std::is_trivially_copyable_v<ymir::state::State>,
              "RollbackState must be trivially copyable for memcpy-based snapshots");

std::filesystem::path GetFallbackLogPath() {
#if defined(_WIN32)
    HMODULE hm = nullptr;
    char modulePath[MAX_PATH]{};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&GetFallbackLogPath), &hm)) {
        if (GetModuleFileNameA(hm, modulePath, static_cast<DWORD>(std::size(modulePath))) > 0) {
            std::filesystem::path p{modulePath};
            // Place log next to the core DLL.
            return p.parent_path() / "ymir_libretro.log";
        }
    }
#endif
    // Fallback to working directory.
    return std::filesystem::current_path() / "ymir_libretro.log";
}

std::filesystem::path GetSaveLogPath() {
    auto base = GetFallbackLogPath();
    return base.parent_path() / "ymir_libretro_savestate.log";
}

void AppendFallbackLog(std::string_view line) {
    std::lock_guard lock{g_logMtx};
    static const auto logPath = GetFallbackLogPath();
    std::ofstream out{logPath, std::ios::app};
    if (out) {
        out << line << '\n';
        out.flush();
    }
}

void AppendSaveLog(std::string_view line) {
    std::lock_guard lock{g_logMtx};
    static const auto logPath = GetSaveLogPath();
    std::ofstream out{logPath, std::ios::app};
    if (out) {
        out << line << '\n';
        out.flush();
    }
}

#if defined(_WIN32)
void SEHTranslator(unsigned int code, EXCEPTION_POINTERS * /*info*/) {
    // Translate Windows SEH to C++ exceptions so we can log them.
    throw std::runtime_error(fmt::format("Structured exception 0x{:X}", code));
}
#endif

void RefreshLogInterface() {
    g_ctx.log_cb = nullptr;
    if (g_ctx.env_cb == nullptr) {
        return;
    }

    retro_log_callback callback{};
    if (g_ctx.env_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &callback) && callback.log != nullptr) {
        g_ctx.log_cb = callback.log;
    }
}

void Log(retro_log_level level, std::string_view message) {
    const auto formatted = fmt::format("[Ymir] {}", message);
    AppendFallbackLog(formatted);
    if (g_ctx.log_cb != nullptr) {
        g_ctx.log_cb(level, "%s", std::string(message).c_str());
    } else {
        fmt::print(stderr, "{}\n", formatted);
#if defined(_WIN32)
        OutputDebugStringA(formatted.c_str());
        OutputDebugStringA("\n");
#endif
    }
}

template <typename... Args>
void LogFmt(retro_log_level level, fmt::format_string<Args...> fmtStr, Args &&...args) {
    const auto formatted = fmt::format(fmtStr, std::forward<Args>(args)...);
    AppendFallbackLog("[Ymir] " + formatted);
    if (g_ctx.log_cb != nullptr) {
        g_ctx.log_cb(level, "%s", formatted.c_str());
    } else {
        fmt::print(stderr, "[Ymir] {}\n", formatted);
#if defined(_WIN32)
        OutputDebugStringA(("[Ymir] " + formatted + "\n").c_str());
#endif
    }
}

void DisplayFrontendMessage(std::string_view message) {
    if (g_ctx.env_cb == nullptr) {
        return;
    }
    retro_message msg{message.data(), 360};
    g_ctx.env_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
}

std::optional<std::filesystem::path> QueryPath(unsigned cmd) {
    if (g_ctx.env_cb == nullptr) {
        return std::nullopt;
    }
    const char *dir = nullptr;
    if (g_ctx.env_cb(cmd, &dir) && dir != nullptr) {
        return std::filesystem::path{dir};
    }
    return std::nullopt;
}

std::filesystem::path DetermineSavePath() {
    if (const auto save = QueryPath(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY)) {
        return *save;
    }
    if (const auto system = QueryPath(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY)) {
        return *system;
    }
    return std::filesystem::current_path();
}

bool ReadFileExact(const std::filesystem::path &path, std::span<uint8_t> out) {
    std::ifstream in{path, std::ios::binary | std::ios::ate};
    if (!in) {
        return false;
    }
    const auto size = static_cast<size_t>(in.tellg());
    if (size != out.size()) {
        return false;
    }
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<size_t>(in.gcount()) == out.size();
}

std::optional<std::filesystem::path> FindIPLROM(const std::filesystem::path &systemDir) {
    const std::vector<std::filesystem::path> roots = {systemDir / "ymir" / "roms" / "ipl",
                                                      systemDir / "roms" / "ipl", systemDir};
    for (const auto &root : roots) {
        std::error_code ec{};
        if (!std::filesystem::is_directory(root, ec)) {
            continue;
        }
        for (std::filesystem::recursive_directory_iterator it{root, ec}, end; it != end && !ec; ++it) {
            if (!it->is_regular_file()) {
                continue;
            }
            if (it->file_size() == ymir::sys::kIPLSize) {
                LogFmt(RETRO_LOG_INFO, "Ymir: Found BIOS candidate '{}'", it->path().string());
                return it->path();
            }
        }
    }
    return std::nullopt;
}

bool LoadIPLROM(ymir::Saturn &saturn) {
    const auto path = FindIPLROM(g_ctx.system_dir);
    if (!path) {
        Log(RETRO_LOG_ERROR,
            "Ymir: Could not locate a 512 KiB Saturn BIOS (IPL). Add a 512 KiB BIOS under system/ymir/roms/ipl, "
            "system/roms/ipl, or system/.");
        DisplayFrontendMessage(
            "Ymir core: missing BIOS. Place a 512 KiB Saturn IPL under system/ymir/roms/ipl or system/roms/ipl.");
        return false;
    }

    std::array<uint8_t, ymir::sys::kIPLSize> ipl{};
    if (!ReadFileExact(*path, ipl)) {
        LogFmt(RETRO_LOG_ERROR, "Ymir: Failed to read IPL ROM '{}'", path->string());
        return false;
    }

    saturn.LoadIPL(std::span<uint8_t, ymir::sys::kIPLSize>(ipl));
    if (const auto *info = ymir::db::GetIPLROMInfo(saturn.mem.GetIPLHash()); info != nullptr) {
        LogFmt(RETRO_LOG_INFO, "Ymir: Loaded IPL ROM '{}' (region {})", path->string(),
               static_cast<int>(info->region));
    } else {
        LogFmt(RETRO_LOG_INFO, "Ymir: Loaded IPL ROM '{}'", path->string());
    }
    return true;
}

bool LoadPersistentData(ymir::Saturn &saturn) {
    std::error_code ec{};
    std::filesystem::create_directories(g_ctx.save_dir, ec);
    if (ec) {
        LogFmt(RETRO_LOG_WARN, "Ymir: Failed to create save directory '{}': {}", g_ctx.save_dir.string(),
               ec.message());
    }

    g_ctx.backup_ram_path = g_ctx.save_dir / "ymir_internal_backup.bin";
    saturn.mem.LoadInternalBackupMemoryImage(g_ctx.backup_ram_path, ec);
    if (ec) {
        LogFmt(RETRO_LOG_WARN, "Ymir: Could not load internal backup RAM '{}': {}", g_ctx.backup_ram_path.string(),
               ec.message());
    }

    g_ctx.smpc_persist_path = g_ctx.save_dir / "ymir_smpc.dat";
    saturn.SMPC.LoadPersistentDataFrom(g_ctx.smpc_persist_path, ec);
    if (ec) {
        LogFmt(RETRO_LOG_WARN, "Ymir: Could not load SMPC persistent data '{}': {}", g_ctx.smpc_persist_path.string(),
               ec.message());
    }
    return true;
}

bool LoadDisc(ymir::Saturn &saturn, const std::filesystem::path &path) {
    ymir::media::Disc disc{};
    const bool ok =
        ymir::media::LoadDisc(path, disc, false, [](ymir::media::MessageType type, std::string message) {
            switch (type) {
            case ymir::media::MessageType::InvalidFormat:
            case ymir::media::MessageType::NotValid: Log(RETRO_LOG_ERROR, message); break;
            case ymir::media::MessageType::Error: LogFmt(RETRO_LOG_ERROR, "Disc load error: {}", message); break;
            case ymir::media::MessageType::Debug: LogFmt(RETRO_LOG_DEBUG, "{}", message); break;
            }
        });
    if (!ok) {
        LogFmt(RETRO_LOG_ERROR, "Ymir: Failed to load disc '{}'", path.string());
        DisplayFrontendMessage("Ymir core: failed to load disc (see log)");
        return false;
    }

    saturn.LoadDisc(std::move(disc));
    saturn.AutodetectRegion(saturn.GetDisc().header.compatAreaCode);
    return true;
}

void ConfigureCallbacks(ymir::Saturn &saturn) {
    saturn.VDP.SetRenderCallback({&g_ctx, [](uint32_t *fb, uint32_t width, uint32_t height, void *ctx) {
                                      auto &context = *static_cast<LibretroContext *>(ctx);
                                      const size_t pixelCount = static_cast<size_t>(width) * height;
                                      if (context.framebuffer_xbgr.size() < pixelCount) {
                                          context.framebuffer_xbgr.resize(pixelCount);
                                      }
                                      std::copy_n(fb, pixelCount, context.framebuffer_xbgr.data());
                                      context.fb_width = width;
                                      context.fb_height = height;
                                      context.frame_ready = true;
                                  }});

    saturn.SCSP.SetSampleCallback(
        {&g_ctx, [](int16_t left, int16_t right, void *ctx) {
             auto &context = *static_cast<LibretroContext *>(ctx);
             context.audio_buffer.push_back(left);
             context.audio_buffer.push_back(right);
         }});
}

void ConfigureInput(ymir::Saturn &saturn) {
    auto &port1 = saturn.SMPC.GetPeripheralPort1();
    auto &port2 = saturn.SMPC.GetPeripheralPort2();
    port1.ConnectControlPad();
    port2.ConnectControlPad();

    auto readPad = [](ymir::peripheral::PeripheralReport &report, void *ctx) {
        if (g_ctx.input_state_cb == nullptr) {
            return;
        }

        const auto port = static_cast<InputContext *>(ctx)->port;
        auto buttons = Button::Default;
        auto set_button = [&](Button button, unsigned id) {
            const bool pressed =
                g_ctx.input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, id) != 0;
            if (pressed) {
                buttons &= ~button;
            } else {
                buttons |= button;
            }
        };

        set_button(Button::Up, RETRO_DEVICE_ID_JOYPAD_UP);
        set_button(Button::Down, RETRO_DEVICE_ID_JOYPAD_DOWN);
        set_button(Button::Left, RETRO_DEVICE_ID_JOYPAD_LEFT);
        set_button(Button::Right, RETRO_DEVICE_ID_JOYPAD_RIGHT);
        set_button(Button::Start, RETRO_DEVICE_ID_JOYPAD_START);

        set_button(Button::B, RETRO_DEVICE_ID_JOYPAD_B);
        set_button(Button::C, RETRO_DEVICE_ID_JOYPAD_A);
        set_button(Button::A, RETRO_DEVICE_ID_JOYPAD_Y);
        set_button(Button::X, RETRO_DEVICE_ID_JOYPAD_X);
        set_button(Button::Y, RETRO_DEVICE_ID_JOYPAD_L2);
        set_button(Button::Z, RETRO_DEVICE_ID_JOYPAD_R2);
        set_button(Button::L, RETRO_DEVICE_ID_JOYPAD_L);
        set_button(Button::R, RETRO_DEVICE_ID_JOYPAD_R);

        report.type = PeripheralType::ControlPad;
        report.report.controlPad.buttons = buttons;
    };

    port1.SetPeripheralReportCallback({&g_ctx.input_contexts[0], readPad});
    port2.SetPeripheralReportCallback({&g_ctx.input_contexts[1], readPad});
}

void ConfigureOptions(ymir::Saturn &saturn) {
    // Keep callbacks on the main emulation thread for predictable libretro integration.
    saturn.configuration.video.threadedVDP = false;
    saturn.configuration.video.threadedDeinterlacer = false;
    saturn.configuration.video.includeVDP1InRenderThread = false;
    saturn.configuration.audio.threadedSCSP = false;
}

void SendInputDescriptors() {
    if (g_ctx.env_cb == nullptr) {
        return;
    }

    static const retro_input_descriptor descriptors[] = {
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "C"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "A"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "X"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Y"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Z"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "P2 D-Pad Up"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "P2 D-Pad Down"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "P2 D-Pad Left"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "P2 D-Pad Right"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "P2 Start"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "P2 B"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "P2 C"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "P2 A"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "P2 X"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "P2 Y"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "P2 Z"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "P2 L"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "P2 R"},
        {0, RETRO_DEVICE_NONE, 0, 0, nullptr},
    };

    g_ctx.env_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)descriptors);
}

void SendControllerInfo() {
    if (g_ctx.env_cb == nullptr) {
        return;
    }
    static const retro_controller_description pads[] = {{"Sega Saturn Control Pad", RETRO_DEVICE_JOYPAD}};
    static const retro_controller_info ports[] = {{pads, 1}, {pads, 1}, {nullptr, 0}};
    g_ctx.env_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *)ports);
}

retro_system_av_info BuildAVInfo() {
    retro_system_av_info av{};
    av.geometry.base_width = 320;
    av.geometry.base_height = 224;
    av.geometry.max_width = ymir::vdp::kMaxResH;
    av.geometry.max_height = ymir::vdp::kMaxResV;
    av.geometry.aspect_ratio = 4.0f / 3.0f;
    av.timing.fps = (g_ctx.saturn && g_ctx.saturn->GetVideoStandard() == ymir::core::config::sys::VideoStandard::PAL)
                        ? 50.0
                        : 60.0;
    av.timing.sample_rate = kAudioSampleRate;
    return av;
}

bool SerializeToBuffer(std::vector<uint8_t> &out) {
    if (!g_ctx.saturn) {
        return false;
    }
    try {
        AppendSaveLog("[savestate] SerializeToBuffer enter (rollback)");
        Log(RETRO_LOG_INFO, "Ymir: SerializeToBuffer entering (rollback blob)");
        ymir::state::State state{};
        g_ctx.saturn->SaveState(state);
        out.resize(kRollbackStateSize);
        std::memcpy(out.data(), &state, kRollbackStateSize);
        LogFmt(RETRO_LOG_INFO, "Ymir: SerializeToBuffer serialized {} bytes (rollback)", out.size());
        AppendSaveLog(fmt::format("[savestate] SerializeToBuffer done size={} (rollback)", out.size()));
        return true;
    } catch (const std::exception &e) {
        LogFmt(RETRO_LOG_ERROR, "Ymir: Failed to serialize state: {}", e.what());
        AppendSaveLog(std::string("[savestate] SerializeToBuffer exception: ") + e.what());
        return false;
    }
}

bool SerializeToBufferSafe(std::vector<uint8_t> &out) {
    return SerializeToBuffer(out);
}

bool DeserializeFromBuffer(const void *data, size_t size) {
    if (!g_ctx.saturn) {
        return false;
    }
    try {
        if (size < kRollbackStateSize) {
            AppendSaveLog(fmt::format("[savestate] DeserializeFromBuffer too small {} < {}", size, kRollbackStateSize));
            LogFmt(RETRO_LOG_ERROR, "Ymir: DeserializeFromBuffer too small {} < {}", size, kRollbackStateSize);
            return false;
        }

        ymir::state::State state{};
        std::memcpy(&state, data, kRollbackStateSize);
        if (!g_ctx.saturn->LoadState(state, false)) {
            Log(RETRO_LOG_ERROR, "Ymir: Save state does not match the currently loaded disc or BIOS");
            return false;
        }
        return true;
    } catch (const std::exception &e) {
        LogFmt(RETRO_LOG_ERROR, "Ymir: Failed to load save state: {}", e.what());
        return false;
    }
}

bool DeserializeFromBufferSafe(const void *data, size_t size) {
    return DeserializeFromBuffer(data, size);
}

void FlushPersistentData() {
    if (!g_ctx.saturn) {
        return;
    }
    std::error_code ec{};
    g_ctx.saturn->SMPC.SavePersistentDataTo(g_ctx.smpc_persist_path, ec);
    if (ec) {
        LogFmt(RETRO_LOG_WARN, "Ymir: Could not save SMPC persistent data '{}': {}", g_ctx.smpc_persist_path.string(),
               ec.message());
    }
}

} // namespace

extern "C" {

RETRO_API unsigned retro_api_version() {
    Log(RETRO_LOG_INFO, "Ymir: retro_api_version");
    return RETRO_API_VERSION;
}

RETRO_API void retro_set_environment(retro_environment_t cb) {
    g_ctx.env_cb = cb;
    Log(RETRO_LOG_INFO, "Ymir: retro_set_environment");
    RefreshLogInterface();
    SendControllerInfo();
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) {
    g_ctx.video_cb = cb;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) {
    g_ctx.audio_cb = cb;
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
    g_ctx.audio_batch_cb = cb;
}

RETRO_API void retro_set_input_poll(retro_input_poll_t cb) {
    g_ctx.input_poll_cb = cb;
}

RETRO_API void retro_set_input_state(retro_input_state_t cb) {
    g_ctx.input_state_cb = cb;
}

RETRO_API void retro_init() {
    Log(RETRO_LOG_INFO, "Ymir: retro_init");
#if defined(_WIN32)
    _set_se_translator(SEHTranslator);
#endif
    RefreshLogInterface();
    if (g_ctx.env_cb != nullptr) {
        bool noGame = false;
        g_ctx.env_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &noGame);
    }
}

RETRO_API void retro_deinit() {
    g_ctx = LibretroContext{};
}

RETRO_API void retro_get_system_info(retro_system_info *info) {
    if (info == nullptr) {
        return;
    }
    Log(RETRO_LOG_INFO, "Ymir: retro_get_system_info");
    info->library_name = "Ymir";
    info->library_version = ymir::version::string;
    // Accept common Saturn disc image formats; bin/img allow BIN/CUE and CCD/IMG/SUB sets.
    info->valid_extensions = "chd|cue|iso|mds|ccd|bin|img";
    info->need_fullpath = true;
    info->block_extract = true;
}

RETRO_API void retro_get_system_av_info(retro_system_av_info *info) {
    if (info == nullptr) {
        return;
    }
    Log(RETRO_LOG_INFO, "Ymir: retro_get_system_av_info");
    *info = BuildAVInfo();
}

RETRO_API void retro_set_controller_port_device(unsigned /*port*/, unsigned /*device*/) {}

RETRO_API void retro_reset() {
    Log(RETRO_LOG_INFO, "Ymir: retro_reset");
    if (g_ctx.saturn) {
        g_ctx.saturn->Reset(true);
        g_ctx.frame_ready = false;
        g_ctx.audio_buffer.clear();
    }
}

RETRO_API void retro_run() {
    Log(RETRO_LOG_INFO, "Ymir: retro_run");
    if (!g_ctx.saturn) {
        return;
    }

    if (g_ctx.input_poll_cb != nullptr) {
        g_ctx.input_poll_cb();
    }

    g_ctx.saturn->RunFrame();

    if (!g_ctx.audio_buffer.empty()) {
        const auto frames = g_ctx.audio_buffer.size() / 2;
        if (g_ctx.audio_batch_cb != nullptr) {
            g_ctx.audio_batch_cb(g_ctx.audio_buffer.data(), frames);
        } else if (g_ctx.audio_cb != nullptr) {
            for (size_t i = 0; i < frames; ++i) {
                g_ctx.audio_cb(g_ctx.audio_buffer[i * 2], g_ctx.audio_buffer[i * 2 + 1]);
            }
        }
        g_ctx.audio_buffer.clear();
    }

    if (g_ctx.frame_ready && g_ctx.video_cb != nullptr) {
        const size_t pixelCount = static_cast<size_t>(g_ctx.fb_width) * g_ctx.fb_height;
        if (g_ctx.framebuffer_xrgb.size() < pixelCount) {
            g_ctx.framebuffer_xrgb.resize(pixelCount);
        }
        for (size_t i = 0; i < pixelCount; ++i) {
            const uint32_t src = g_ctx.framebuffer_xbgr[i];
            const uint32_t r = src & 0xFF;
            const uint32_t g = (src >> 8) & 0xFF;
            const uint32_t b = (src >> 16) & 0xFF;
            g_ctx.framebuffer_xrgb[i] = (r << 16) | (g << 8) | b;
        }
        g_ctx.video_cb(g_ctx.framebuffer_xrgb.data(), g_ctx.fb_width, g_ctx.fb_height,
                       g_ctx.fb_width * sizeof(uint32_t));
        g_ctx.frame_ready = false;
    } else if (g_ctx.video_cb != nullptr) {
        g_ctx.video_cb(nullptr, 0, 0, 0);
    }
}

RETRO_API size_t retro_serialize_size() {
    LogFmt(RETRO_LOG_INFO, "Ymir: retro_serialize_size -> {}", kRollbackStateSize);
    AppendSaveLog(fmt::format("[savestate] retro_serialize_size -> {} (rollback fixed size)", kRollbackStateSize));
    return kRollbackStateSize;
}

RETRO_API bool retro_serialize(void *data, size_t size) {
    try {
        LogFmt(RETRO_LOG_INFO, "Ymir: retro_serialize requested size {}", size);
        AppendSaveLog(fmt::format("[savestate] retro_serialize requested size {}", size));
        if (size < kRollbackStateSize) {
            LogFmt(RETRO_LOG_ERROR, "Ymir: retro_serialize buffer too small (need {}, have {})", kRollbackStateSize, size);
            AppendSaveLog(fmt::format("[savestate] retro_serialize buffer too small need {} have {}", kRollbackStateSize, size));
            return false;
        }
        std::vector<uint8_t> buffer{};
        if (!SerializeToBufferSafe(buffer)) {
            return false;
        }
        if (buffer.size() != kRollbackStateSize) {
            LogFmt(RETRO_LOG_WARN, "Ymir: retro_serialize unexpected blob size {} (expected {})", buffer.size(), kRollbackStateSize);
        }
        std::memcpy(data, buffer.data(), kRollbackStateSize);
        LogFmt(RETRO_LOG_INFO, "Ymir: retro_serialize wrote {} bytes (rollback)", kRollbackStateSize);
        AppendSaveLog(fmt::format("[savestate] retro_serialize wrote {} bytes (rollback)", kRollbackStateSize));
        return true;
    } catch (const std::exception &e) {
        LogFmt(RETRO_LOG_ERROR, "Ymir: retro_serialize failed: {}", e.what());
        AppendSaveLog(std::string("[savestate] retro_serialize exception: ") + e.what());
    } catch (...) {
        Log(RETRO_LOG_ERROR, "Ymir: retro_serialize failed with unknown error");
        AppendSaveLog("[savestate] retro_serialize exception: unknown");
    }
    return false;
}

RETRO_API bool retro_unserialize(const void *data, size_t size) {
    try {
        LogFmt(RETRO_LOG_INFO, "Ymir: retro_unserialize size {}", size);
        AppendSaveLog(fmt::format("[savestate] retro_unserialize size {}", size));
        if (size < kRollbackStateSize) {
            LogFmt(RETRO_LOG_ERROR, "Ymir: retro_unserialize buffer too small (need {}, have {})", kRollbackStateSize, size);
            AppendSaveLog(fmt::format("[savestate] retro_unserialize buffer too small need {} have {}", kRollbackStateSize, size));
            return false;
        }
        return DeserializeFromBufferSafe(data, size);
    } catch (const std::exception &e) {
        LogFmt(RETRO_LOG_ERROR, "Ymir: retro_unserialize failed: {}", e.what());
        AppendSaveLog(std::string("[savestate] retro_unserialize exception: ") + e.what());
    } catch (...) {
        Log(RETRO_LOG_ERROR, "Ymir: retro_unserialize failed with unknown error");
        AppendSaveLog("[savestate] retro_unserialize exception: unknown");
    }
    return false;
}

RETRO_API bool retro_load_game(const retro_game_info *game) {
    Log(RETRO_LOG_INFO, "Ymir: retro_load_game entered");
    if (game == nullptr || game->path == nullptr) {
        Log(RETRO_LOG_ERROR, "Ymir: retro_load_game called with null game or path");
        return false;
    }

    g_ctx.system_dir = QueryPath(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY).value_or(std::filesystem::current_path());
    g_ctx.save_dir = DetermineSavePath();
    LogFmt(RETRO_LOG_INFO, "Ymir: System dir '{}', Save dir '{}', Loading '{}'", g_ctx.system_dir.string(),
           g_ctx.save_dir.string(), game->path);

    g_ctx.saturn = std::make_unique<ymir::Saturn>();
    ConfigureOptions(*g_ctx.saturn);
    ConfigureCallbacks(*g_ctx.saturn);
    ConfigureInput(*g_ctx.saturn);
    SendInputDescriptors();

    retro_pixel_format pixelFormat = RETRO_PIXEL_FORMAT_XRGB8888;
    if (g_ctx.env_cb != nullptr) {
        g_ctx.env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pixelFormat);
        auto av = BuildAVInfo();
        g_ctx.env_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
    }

    g_ctx.framebuffer_xbgr.resize(ymir::vdp::kMaxResH * ymir::vdp::kMaxResV);
    g_ctx.framebuffer_xrgb.resize(ymir::vdp::kMaxResH * ymir::vdp::kMaxResV);
    g_ctx.audio_buffer.reserve(static_cast<size_t>(kAudioSampleRate) * 2);
    g_ctx.frame_ready = false;
    g_ctx.audio_buffer.clear();

    if (!LoadIPLROM(*g_ctx.saturn)) {
        Log(RETRO_LOG_ERROR, "Ymir: Failed to load BIOS (place a 512 KiB Saturn IPL in the system directory)");
        return false;
    }

    if (!LoadPersistentData(*g_ctx.saturn)) {
        Log(RETRO_LOG_WARN, "Ymir: Failed to bind persistent data paths");
    }

    if (!LoadDisc(*g_ctx.saturn, std::filesystem::path{game->path})) {
        LogFmt(RETRO_LOG_ERROR, "Ymir: Failed to load disc '{}'", game->path);
        return false;
    }

    Log(RETRO_LOG_INFO, "Ymir: Game loaded successfully");
    return true;
}

RETRO_API bool retro_load_game_special(unsigned /*game_type*/, const retro_game_info *info, size_t num_info) {
    if (num_info == 0 || info == nullptr) {
        return false;
    }
    return retro_load_game(info);
}

RETRO_API void retro_unload_game() {
    FlushPersistentData();
    g_ctx.saturn.reset();
    g_ctx.framebuffer_xbgr.clear();
    g_ctx.framebuffer_xrgb.clear();
    g_ctx.audio_buffer.clear();
    g_ctx.frame_ready = false;
}

RETRO_API unsigned retro_get_region() {
    if (g_ctx.saturn && g_ctx.saturn->GetVideoStandard() == ymir::core::config::sys::VideoStandard::PAL) {
        return RETRO_REGION_PAL;
    }
    return RETRO_REGION_NTSC;
}

RETRO_API void *retro_get_memory_data(unsigned /*id*/) {
    return nullptr;
}

RETRO_API size_t retro_get_memory_size(unsigned /*id*/) {
    return 0;
}

RETRO_API void retro_cheat_reset() {}

RETRO_API void retro_cheat_set(unsigned /*index*/, bool /*enabled*/, const char * /*code*/) {}

} // extern "C"
