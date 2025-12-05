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
find_package(cereal CONFIG REQUIRED)

target_link_libraries(ymir-libretro
    PRIVATE
        ymir-core
        fmt::fmt
        cereal::cereal
)

set_target_properties(ymir-libretro
    PROPERTIES
        OUTPUT_NAME "ymir_libretro"
        PREFIX ""
)
