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

# hiredis: prefer CMake config (Trixie libhiredis-dev 1.x); fall back to
# pkg-config, exposing a uniform hiredis::hiredis target either way.
find_package(hiredis QUIET CONFIG)
if(NOT TARGET hiredis::hiredis)
    pkg_check_modules(HIREDIS REQUIRED hiredis)
    add_library(hiredis::hiredis INTERFACE IMPORTED GLOBAL)
    target_include_directories(hiredis::hiredis INTERFACE ${HIREDIS_INCLUDE_DIRS})
    target_link_libraries(hiredis::hiredis INTERFACE ${HIREDIS_LIBRARIES})
    if(HIREDIS_LIBRARY_DIRS)
        target_link_directories(hiredis::hiredis INTERFACE ${HIREDIS_LIBRARY_DIRS})
    endif()
    message(STATUS "hiredis: using pkg-config (${HIREDIS_VERSION})")
endif()

include_directories(${DRM_INCLUDE_DIRS})

add_subdirectory(lib/lvgl)

add_executable(kdeskdash
    src/main.c
    src/config.c
    src/registry.c
    src/shell.c
    src/gol.c
    src/golz.c
    src/stopwatch.c
    src/redis.c
    src/dev_telemetry.c
    src/telemetry.c
    src/telemetry_host.c
    src/claude_feed.c
    src/claude_redis.c
    src/bmp_write.c
    src/screenshot.c
    lib/cjson/cJSON.c
    src/modes/game_of_life.c
    src/modes/golz.c
    src/modes/clock.c
    src/modes/dev.c
    src/modes/dev_graph.c
    src/modes/dev_hostlist.c
    src/modes/dev_view.c
    src/modes/claude.c
    src/modes/menu.c
)

target_include_directories(kdeskdash PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/lib/cjson
    ${DRM_INCLUDE_DIRS}
)

target_link_libraries(kdeskdash
    lvgl
    ${DRM_LIBRARIES}
    hiredis::hiredis
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
# Stops the service (or a manually-run instance), copies the binary, restarts it.
set(KDESKDASH_TARGET "ken@rpidash2" CACHE STRING "ssh target for deploy")
add_custom_target(deploy
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/deploy.sh deploy ${KDESKDASH_TARGET} ${CMAKE_BINARY_DIR}/kdeskdash
    DEPENDS kdeskdash
    COMMENT "Deploying kdeskdash to ${KDESKDASH_TARGET}"
)

# One-time service setup: cmake --build build-pi --target install-service
# Installs the systemd unit and an env file (only if absent), reloads, and
# enables the service. Safe to re-run; it will not overwrite an edited env file.
add_custom_target(install-service
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/deploy.sh install-service ${KDESKDASH_TARGET} ${CMAKE_SOURCE_DIR}/deploy/kdeskdash.service ${CMAKE_SOURCE_DIR}/deploy/kdeskdash.env.example
    COMMENT "Installing kdeskdash systemd service on ${KDESKDASH_TARGET}"
)

# Host-only unit tests for pure logic (no LVGL/DRM). Run with: ctest --test-dir build
# Skipped when cross-compiling, since ctest executes the binaries on the build host.
if(NOT CMAKE_CROSSCOMPILING)
    enable_testing()

    add_executable(test_registry tests/test_registry.c src/registry.c)
    target_include_directories(test_registry PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_registry PRIVATE -Wall -Wextra)
    add_test(NAME test_registry COMMAND test_registry)

    add_executable(test_gol tests/test_gol.c src/gol.c)
    target_include_directories(test_gol PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_gol PRIVATE -Wall -Wextra)
    add_test(NAME test_gol COMMAND test_gol)

    add_executable(test_golz tests/test_golz.c src/golz.c src/gol.c)
    target_include_directories(test_golz PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_golz PRIVATE -Wall -Wextra)
    add_test(NAME test_golz COMMAND test_golz)

    # GoLZ machete balance sweep (headless, pure core). Not a ctest — built on
    # demand: cmake --build build --target golz_mc, then ./build/golz_mc --help.
    add_executable(golz_mc EXCLUDE_FROM_ALL tools/golz_mc.c src/golz.c src/gol.c)
    target_include_directories(golz_mc PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(golz_mc PRIVATE -Wall -Wextra -O2)

    add_executable(test_stopwatch tests/test_stopwatch.c src/stopwatch.c)
    target_include_directories(test_stopwatch PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_stopwatch PRIVATE -Wall -Wextra)
    add_test(NAME test_stopwatch COMMAND test_stopwatch)

    # dev_telemetry parser pulls in vendored cJSON (needs lib/cjson on the
    # include path and libm for cJSON's number handling).
    add_executable(test_dev_telemetry
        tests/test_dev_telemetry.c
        src/dev_telemetry.c
        lib/cjson/cJSON.c
    )
    target_include_directories(test_dev_telemetry PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/lib/cjson
    )
    target_compile_options(test_dev_telemetry PRIVATE -Wall -Wextra)
    target_link_libraries(test_dev_telemetry m)
    add_test(NAME test_dev_telemetry COMMAND test_dev_telemetry)

    # Pure BMP encoder (device self-screenshot output; LVGL glue not tested).
    add_executable(test_bmp_write tests/test_bmp_write.c src/bmp_write.c)
    target_include_directories(test_bmp_write PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_bmp_write PRIVATE -Wall -Wextra)
    add_test(NAME test_bmp_write COMMAND test_bmp_write)

    # Pure claude-feed core: key contract, hash parsing, status ladder, sort,
    # limits, recent records, formatting (cJSON + libm like dev_telemetry).
    add_executable(test_claude_feed
        tests/test_claude_feed.c
        src/claude_feed.c
        src/telemetry_host.c
        lib/cjson/cJSON.c
    )
    target_include_directories(test_claude_feed PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/lib/cjson
    )
    target_compile_options(test_claude_feed PRIVATE -Wall -Wextra)
    target_link_libraries(test_claude_feed m)
    add_test(NAME test_claude_feed COMMAND test_claude_feed)

    # Pure host-token contract for telemetry discovery keys (no Redis/LVGL).
    add_executable(test_telemetry_host
        tests/test_telemetry_host.c
        src/telemetry_host.c
    )
    target_include_directories(test_telemetry_host PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_telemetry_host PRIVATE -Wall -Wextra)
    add_test(NAME test_telemetry_host COMMAND test_telemetry_host)

    # Pure host-selector list model: lexicographic sort + drop/re-add debounce
    # + keep-selected (no LVGL/Redis).
    add_executable(test_dev_hostlist
        tests/test_dev_hostlist.c
        src/modes/dev_hostlist.c
    )
    target_include_directories(test_dev_hostlist PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_dev_hostlist PRIVATE -Wall -Wextra)
    add_test(NAME test_dev_hostlist COMMAND test_dev_hostlist)

    # Pure dev-mode view logic: liveness precedence ladder + CPU-only debounce.
    add_executable(test_dev_view
        tests/test_dev_view.c
        src/modes/dev_view.c
    )
    target_include_directories(test_dev_view PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_dev_view PRIVATE -Wall -Wextra)
    add_test(NAME test_dev_view COMMAND test_dev_view)
endif()
