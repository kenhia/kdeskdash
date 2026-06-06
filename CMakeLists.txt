cmake_minimum_required(VERSION 3.22)
project(kdeskdash C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# LVGL finds lv_conf.h via this include path
set(LV_CONF_PATH "${CMAKE_SOURCE_DIR}/lv_conf.h" CACHE STRING "Path to lv_conf.h")
add_compile_definitions(LV_CONF_PATH=${LV_CONF_PATH})

# Generated image C files include "lvgl.h" when this is defined
add_compile_definitions(LV_LVGL_H_INCLUDE_SIMPLE)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DRM REQUIRED libdrm)

include_directories(${DRM_INCLUDE_DIRS})

add_subdirectory(lib/lvgl)

add_executable(kdeskdash
    src/main.c
    src/config.c
    src/registry.c
    src/shell.c
    src/modes/placeholder.c
    src/demo_screen.c
    assets/brain_rot.c
)

target_include_directories(kdeskdash PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${DRM_INCLUDE_DIRS}
)

target_link_libraries(kdeskdash
    lvgl
    ${DRM_LIBRARIES}
    pthread
)

target_compile_options(kdeskdash PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Cross-linking against a Trixie sysroot (glibc 2.38) from an older dev host:
# shared libs like libdrm reference GLIBC_2.38 symbols the host stub libc
# doesn't define. Allow them at link time; they resolve at runtime on the Pi.
if(CMAKE_CROSSCOMPILING)
    target_link_options(kdeskdash PRIVATE -Wl,--allow-shlib-undefined)
endif()

# Deploy to the Pi: cmake --build build-pi --target deploy
set(KDESKDASH_TARGET "ken@rpidash2" CACHE STRING "ssh target for deploy")
add_custom_target(deploy
    COMMAND scp ${CMAKE_BINARY_DIR}/kdeskdash ${KDESKDASH_TARGET}:~/kdeskdash
    DEPENDS kdeskdash
    COMMENT "Deploying kdeskdash to ${KDESKDASH_TARGET}"
)

# Host-only unit tests for pure logic (no LVGL/DRM). Run with: ctest --test-dir build
# Skipped when cross-compiling, since ctest executes the binaries on the build host.
if(NOT CMAKE_CROSSCOMPILING)
    enable_testing()

    add_executable(test_registry tests/test_registry.c src/registry.c)
    target_include_directories(test_registry PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_registry PRIVATE -Wall -Wextra)
    add_test(NAME test_registry COMMAND test_registry)
endif()
