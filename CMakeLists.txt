cmake_minimum_required(VERSION 3.13)

include($ENV{DEVKITPRO}/cmake/3DS.cmake)
list(APPEND CMAKE_FIND_ROOT_PATH $ENV{DEVKITPRO}/portlibs/3ds)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(jamgame LANGUAGES C VERSION 1.0.0)
add_executable(jamgame
    source/main.c
    source/cameraButton.c
    source/cameraButton.h
)
target_link_libraries(jamgame PRIVATE
    citro2d
    citro3d
)

target_compile_options(jamgame PRIVATE
    -O3
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-function
    -Wdouble-promotion
    -Wno-strict-prototypes
)

ctr_generate_smdh(jamgame.smdh
    NAME        "jamgame"
    DESCRIPTION "wawa"
    AUTHOR      "Gingeh/April"
    VERSION     "${PROJECT_VERSION}"
)
ctr_create_3dsx(jamgame SMDH jamgame.smdh)
