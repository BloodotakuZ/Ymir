#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RETRO_CALLCONV
    #if defined(_MSC_VER)
        #define RETRO_CALLCONV __cdecl
    #else
        #define RETRO_CALLCONV
    #endif
#endif

#ifndef RETRO_API
    #if defined(_WIN32)
        #define RETRO_API __declspec(dllexport)
    #else
        #define RETRO_API
    #endif
#endif

#define RETRO_API_VERSION 1

enum retro_pixel_format {
    RETRO_PIXEL_FORMAT_0RGB1555 = 0,
    RETRO_PIXEL_FORMAT_XRGB8888 = 1,
    RETRO_PIXEL_FORMAT_RGB565 = 2,
    RETRO_PIXEL_FORMAT_UNKNOWN = 0xFFFFFFFF
};

enum retro_region { RETRO_REGION_NTSC = 0, RETRO_REGION_PAL = 1 };

enum retro_device {
    RETRO_DEVICE_NONE = 0,
    RETRO_DEVICE_JOYPAD = 1,
    RETRO_DEVICE_MOUSE = 2,
    RETRO_DEVICE_KEYBOARD = 3,
    RETRO_DEVICE_LIGHTGUN = 4,
    RETRO_DEVICE_ANALOG = 5,
    RETRO_DEVICE_POINTER = 6
};

enum retro_device_id_joypad {
    RETRO_DEVICE_ID_JOYPAD_B = 0,
    RETRO_DEVICE_ID_JOYPAD_Y = 1,
    RETRO_DEVICE_ID_JOYPAD_SELECT = 2,
    RETRO_DEVICE_ID_JOYPAD_START = 3,
    RETRO_DEVICE_ID_JOYPAD_UP = 4,
    RETRO_DEVICE_ID_JOYPAD_DOWN = 5,
    RETRO_DEVICE_ID_JOYPAD_LEFT = 6,
    RETRO_DEVICE_ID_JOYPAD_RIGHT = 7,
    RETRO_DEVICE_ID_JOYPAD_A = 8,
    RETRO_DEVICE_ID_JOYPAD_X = 9,
    RETRO_DEVICE_ID_JOYPAD_L = 10,
    RETRO_DEVICE_ID_JOYPAD_R = 11,
    RETRO_DEVICE_ID_JOYPAD_L2 = 12,
    RETRO_DEVICE_ID_JOYPAD_R2 = 13,
    RETRO_DEVICE_ID_JOYPAD_L3 = 14,
    RETRO_DEVICE_ID_JOYPAD_R3 = 15
};

enum retro_memory {
    RETRO_MEMORY_NONE = 0,
    RETRO_MEMORY_SYSTEM_RAM = 1,
    RETRO_MEMORY_SAVE_RAM = 3,
    RETRO_MEMORY_VIDEO_RAM = 6
};

enum retro_log_level { RETRO_LOG_DEBUG = 0, RETRO_LOG_INFO, RETRO_LOG_WARN, RETRO_LOG_ERROR, RETRO_LOG_FATAL };

typedef void (RETRO_CALLCONV *retro_log_printf_t)(int level, const char *fmt, ...);

struct retro_log_callback {
    retro_log_printf_t log;
};

struct retro_message {
    const char *msg;
    unsigned frames;
};

struct retro_game_info {
    const char *path;
    const void *data;
    size_t size;
    const char *meta;
};

struct retro_system_info {
    const char *library_name;
    const char *library_version;
    const char *valid_extensions;
    bool need_fullpath;
    bool block_extract;
};

struct retro_game_geometry {
    unsigned base_width;
    unsigned base_height;
    unsigned max_width;
    unsigned max_height;
    float aspect_ratio;
};

struct retro_system_timing {
    double fps;
    double sample_rate;
};

struct retro_system_av_info {
    struct retro_game_geometry geometry;
    struct retro_system_timing timing;
};

struct retro_input_descriptor {
    unsigned port;
    unsigned device;
    unsigned index;
    unsigned id;
    const char *description;
};

struct retro_controller_description {
    const char *desc;
    unsigned id;
};

struct retro_controller_info {
    const struct retro_controller_description *types;
    unsigned num_types;
};

struct retro_variable {
    const char *key;
    const char *value;
};

enum retro_environment {
    RETRO_ENVIRONMENT_SET_ROTATION = 1,
    RETRO_ENVIRONMENT_GET_OVERSCAN = 2,
    RETRO_ENVIRONMENT_GET_CAN_DUPE = 3,
    RETRO_ENVIRONMENT_SET_MESSAGE = 6,
    RETRO_ENVIRONMENT_SHUTDOWN = 7,
    RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL = 8,
    RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY = 9,
    RETRO_ENVIRONMENT_SET_PIXEL_FORMAT = 10,
    RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS = 11,
    RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK = 12,
    RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE = 13,
    RETRO_ENVIRONMENT_SET_HW_RENDER = 14,
    RETRO_ENVIRONMENT_GET_VARIABLE = 15,
    RETRO_ENVIRONMENT_SET_VARIABLES = 16,
    RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE = 17,
    RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME = 18,
    RETRO_ENVIRONMENT_GET_LIBRETRO_PATH = 19,
    RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK = 21,
    RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK = 22,
    RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE = 23,
    RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES = 24,
    RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE = 25,
    RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE = 26,
    RETRO_ENVIRONMENT_GET_LOG_INTERFACE = 27,
    RETRO_ENVIRONMENT_GET_PERF_INTERFACE = 28,
    RETRO_ENVIRONMENT_GET_LOCATION_INTERFACE = 29,
    RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY = 30,
    RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY = 31,
    RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO = 32,
    RETRO_ENVIRONMENT_SET_PROC_ADDRESS_CALLBACK = 33,
    RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO = 34,
    RETRO_ENVIRONMENT_SET_CONTROLLER_INFO = 35,
    RETRO_ENVIRONMENT_SET_MEMORY_MAPS = 36,
    RETRO_ENVIRONMENT_SET_GEOMETRY = 37,
    RETRO_ENVIRONMENT_GET_USERNAME = 38,
    RETRO_ENVIRONMENT_GET_LANGUAGE = 39,
    RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER = 40,
    RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE = 41,
    RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS = 42,
    RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE = 43,
    RETRO_ENVIRONMENT_GET_FASTFORWARDING = 44,
    RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE = 45,
    RETRO_ENVIRONMENT_GET_INPUT_BITMASKS = 46,
    RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION = 47,
    RETRO_ENVIRONMENT_SET_CORE_OPTIONS = 48,
    RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL = 49,
    RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY = 50,
};

typedef bool (RETRO_CALLCONV *retro_environment_t)(unsigned cmd, void *data);
typedef void (RETRO_CALLCONV *retro_video_refresh_t)(const void *data, unsigned width, unsigned height, size_t pitch);
typedef void (RETRO_CALLCONV *retro_audio_sample_t)(int16_t left, int16_t right);
typedef size_t (RETRO_CALLCONV *retro_audio_sample_batch_t)(const int16_t *data, size_t frames);
typedef void (RETRO_CALLCONV *retro_input_poll_t)(void);
typedef int16_t (RETRO_CALLCONV *retro_input_state_t)(unsigned port, unsigned device, unsigned index, unsigned id);

RETRO_API void retro_set_environment(retro_environment_t cb);
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb);
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb);
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb);
RETRO_API void retro_set_input_poll(retro_input_poll_t cb);
RETRO_API void retro_set_input_state(retro_input_state_t cb);

RETRO_API void retro_init(void);
RETRO_API void retro_deinit(void);
RETRO_API unsigned retro_api_version(void);

RETRO_API void retro_get_system_info(struct retro_system_info *info);
RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info);
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device);
RETRO_API void retro_reset(void);
RETRO_API void retro_run(void);

RETRO_API size_t retro_serialize_size(void);
RETRO_API bool retro_serialize(void *data, size_t size);
RETRO_API bool retro_unserialize(const void *data, size_t size);

RETRO_API bool retro_load_game(const struct retro_game_info *game);
RETRO_API bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info);
RETRO_API void retro_unload_game(void);
RETRO_API unsigned retro_get_region(void);

RETRO_API void *retro_get_memory_data(unsigned id);
RETRO_API size_t retro_get_memory_size(unsigned id);

RETRO_API void retro_cheat_reset(void);
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code);

#ifdef __cplusplus
} // extern "C"
#endif

