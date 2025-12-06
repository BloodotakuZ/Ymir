add_library(ymir-libretro SHARED
    src/libretro.cpp
    src/libretro.h
)

target_include_directories(ymir-libretro
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/apps/ymir-sdl3/src
)

target_compile_features(ymir-libretro PRIVATE cxx_std_20)

find_package(fmt CONFIG REQUIRED)

# Enable SEH translation to C++ exceptions on MSVC so we can log structured faults during savestate operations.
if (MSVC)
    target_compile_options(ymir-libretro PRIVATE /EHa)
endif ()

target_link_libraries(ymir-libretro
    PRIVATE
        ymir-core
        fmt::fmt
)

set_target_properties(ymir-libretro
    PROPERTIES
        OUTPUT_NAME "ymir_libretro"
        PREFIX ""
)

# Copy the .info file next to the built core so it is easy to deploy to RetroArch.
add_custom_command(TARGET ymir-libretro POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/ymir_libretro.info
            $<TARGET_FILE_DIR:ymir-libretro>/ymir_libretro.info
    COMMENT "Copying ymir_libretro.info next to the built core")
