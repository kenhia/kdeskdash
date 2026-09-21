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

# --- libkdash --------------------------------------------------------------
# kdashdata's shared C consumer library: the typed readers for the `claude:*`
# family. This panel carried its own copy of that logic until sprint 034
# (src/claude_feed.c + src/claude_redis.c, now deleted), which is the
# duplication CD-10 exists to prevent.
#
# A submodule for the same reasons LVGL is one — the pin is a commit in this
# repo's history and `cmake -B build` needs no network — and following
# kstudiodash, which adopted the library this way first.
#
# KDASH_HIREDIS_STATIC is deliberately NOT set here. kstudiodash forces it ON
# because the machine it deploys to has no libhiredis at all; both Pis already
# run a kdeskdash that links libhiredis.so, so forcing static would change this
# project's deploy story as a side effect of a refactor. hiredis::hiredis is
# created above and kdashdata's own lookup is guarded by `if(NOT TARGET ...)`,
# so libkdash links exactly the hiredis this executable already linked.
#
# Both trees vendor cJSON 1.7.18, byte-identical (checked). kdeskdash compiles
# its copy into the executable and kdash_core archives its own; the linker
# satisfies kdash_core's references from the executable's objects and never
# pulls the archived copy, so there is one cJSON in the binary either way.
#
# KDASH_BUILD_EXAMPLES and KDASH_BUILD_TESTS default OFF when kdashdata is not
# the top-level project, so kdash_dump and its ctest registrations stay out of
# this build without being named here.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/lib/kdashdata/CMakeLists.txt")
    message(FATAL_ERROR
        "lib/kdashdata is empty. Run: git submodule update --init --recursive")
endif()
add_subdirectory(lib/kdashdata)

add_executable(kdeskdash
    src/main.c
    src/config.c
    src/registry.c
    src/shell.c
    src/modeset.c
    src/calc.c
    src/palette.c
    src/clock_core.c
    src/clock_widget.c
    src/gol.c
    src/gol_settings.c
    src/golz.c
    src/iconset.c
    src/stopwatch.c
    src/redis.c
    src/panel_state.c
    src/panel_store.c
    src/panel_cmd.c
    src/panel_feed.c
    src/dev_telemetry.c
    src/telemetry.c
    src/telemetry_host.c
    src/quickswitch.c
    src/service_card.c
    src/service_pub.c
    src/kvscf_feed.c
    src/kvscf_redis.c
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
    src/modes/claude_view.c
    src/modes/icons.c
    src/modes/foreground.c
    src/modes/launcher.c
    src/modes/calc.c
    src/modes/palette.c
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
    kdash        # the claude:* feed readers (lib/kdashdata); opens its own socket
    ${DRM_LIBRARIES}
    hiredis::hiredis
    pthread
    m           # libm: TinyTTF's stb_truetype needs sqrt/floor/pow/... (icons mode)
)

target_compile_options(kdeskdash PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Version stamp, printed by `kdeskdash --version`. The recipe that builds
# passes it (`-DKD_VERSION="$(scripts/version.sh)"`); CMake never derives it,
# so a stale configure cache cannot make the binary claim the wrong commit.
# `unknown` is what a hand-run `cmake -B build` without the flag produces —
# a build that names no commit, and one `just deploy` refuses to install.
set(KD_VERSION "unknown" CACHE STRING "Version string stamped into the binary")
target_compile_definitions(kdeskdash PRIVATE KD_VERSION="${KD_VERSION}")

# Cross-linking against a Trixie sysroot (glibc 2.38) from an older dev host:
# shared libs like libdrm reference GLIBC_2.38 symbols the host stub libc
# doesn't define. Allow them at link time; they resolve at runtime on the Pi.
if(CMAKE_CROSSCOMPILING)
    target_link_options(kdeskdash PRIVATE -Wl,--allow-shlib-undefined)
endif()

# The `deploy` / `install-service` CMake custom targets (and the
# KDESKDASH_TARGET cache variable that aimed them) were retired in sprint 024.
# They copied the binary straight out of this build tree to a board, which is
# precisely the shipping habit k-homelab docs/deploying.md exists to end: no
# history, and no previous build to fall back to when one is bad.
#
# CMake now only builds. Shipping is `just publish` (a versioned artifact in
# the package store) and `just deploy <host> [version]` (installs *that*
# artifact), with `just push-dev <host>` as the labelled dev-loop exception.

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

    # Untrusted settings validation for BOTH settings structs, extracted from
    # redis.c so it can be exercised on the host with no hiredis dependency
    # (guards the cell_size floor). gol_settings.c declares the GoLZ applier
    # too since sprint 039, so golz.c/gol.c come along for golz_settings_t.
    add_executable(test_settings tests/test_settings.c src/gol_settings.c
                   src/golz.c src/gol.c)
    target_include_directories(test_settings PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_settings PRIVATE -Wall -Wextra)
    add_test(NAME test_settings COMMAND test_settings)

    add_executable(test_golz tests/test_golz.c src/golz.c src/gol.c)
    target_include_directories(test_golz PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_golz PRIVATE -Wall -Wextra)
    add_test(NAME test_golz COMMAND test_golz)

    # GoLZ machete balance sweep (headless, pure core). Not a ctest — built on
    # demand: cmake --build build --target golz_mc, then ./build/golz_mc --help.
    add_executable(golz_mc EXCLUDE_FROM_ALL tools/golz_mc.c src/golz.c src/gol.c)
    target_include_directories(golz_mc PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(golz_mc PRIVATE -Wall -Wextra -O2)

    # Pure calculator core: entry editing, immediate-exec ops, registers,
    # error recovery, hex/bin readouts, unit conversions (needs libm for pow).
    add_executable(test_calc tests/test_calc.c src/calc.c)
    target_include_directories(test_calc PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_calc PRIVATE -Wall -Wextra)
    target_link_libraries(test_calc m)
    add_test(NAME test_calc COMMAND test_calc)

    # Canonical named palette: table integrity + case-insensitive lookup.
    add_executable(test_palette tests/test_palette.c src/palette.c)
    target_include_directories(test_palette PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_palette PRIVATE -Wall -Wextra)
    add_test(NAME test_palette COMMAND test_palette)

    # Per-device mode selection: the KDESKDASH_MODES grammar, the built-in
    # default/roster, and every way a malformed spec has to degrade safely.
    add_executable(test_modeset tests/test_modeset.c src/modeset.c)
    target_include_directories(test_modeset PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_modeset PRIVATE -Wall -Wextra)
    add_test(NAME test_modeset COMMAND test_modeset)

    # Pure core behind the shared dual-clock widget: face formatting across DST
    # and the date line, and the type-scale thresholds (no LVGL).
    add_executable(test_clock_core tests/test_clock_core.c src/clock_core.c)
    target_include_directories(test_clock_core PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_clock_core PRIVATE -Wall -Wextra)
    add_test(NAME test_clock_core COMMAND test_clock_core)

    add_executable(test_stopwatch tests/test_stopwatch.c src/stopwatch.c)
    target_include_directories(test_stopwatch PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_stopwatch PRIVATE -Wall -Wextra)
    add_test(NAME test_stopwatch COMMAND test_stopwatch)

    # Pure icons-mode model: set table + splits, paging math, favourites set and
    # the bake-ready favourites file round-trip (no LVGL/TinyTTF).
    add_executable(test_iconset tests/test_iconset.c src/iconset.c)
    target_include_directories(test_iconset PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_iconset PRIVATE -Wall -Wextra)
    add_test(NAME test_iconset COMMAND test_iconset)

    # Pure foreground-mode (kvscf) core: JSON parse, ci-label sort, paging, app
    # colours, focus payload, token trim (cJSON + libm like dev_telemetry) —
    # plus the launcher grid, press payload and no-tofu label filter, which is
    # why the named palette is linked in (buttons may be authored by colour name).
    add_executable(test_kvscf_feed
        tests/test_kvscf_feed.c
        src/kvscf_feed.c
        src/telemetry_host.c
        src/palette.c
        lib/cjson/cJSON.c
    )
    target_include_directories(test_kvscf_feed PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/lib/cjson
    )
    target_compile_options(test_kvscf_feed PRIVATE -Wall -Wextra)
    target_link_libraries(test_kvscf_feed m)
    add_test(NAME test_kvscf_feed COMMAND test_kvscf_feed)

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

    # Panel-side rendering for the claude feed. The feed's CONTRACT — key
    # grammar, hash parsing, the display ladder, the attention-first sort,
    # limits staleness — belongs to libkdash since sprint 034; what is left
    # here is the part CD-10 keeps on the panel, and this is its test. Links
    # kdash_core (pure, no socket) for the display-state enum it labels.
    add_executable(test_claude_view
        tests/test_claude_view.c
        src/modes/claude_view.c
    )
    target_include_directories(test_claude_view PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_options(test_claude_view PRIVATE -Wall -Wextra)
    target_link_libraries(test_claude_view kdash_core)
    add_test(NAME test_claude_view COMMAND test_claude_view)

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

    # Pure double-tap quick-switch core: tap timing (including the LVGL tick
    # wrap), configured pairs, and the most-recent fallback. LVGL 9.2.2 has no
    # double-click event, so the timing is ours and is tested here.
    add_executable(test_quickswitch
        tests/test_quickswitch.c
        src/quickswitch.c
    )
    target_include_directories(test_quickswitch PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_quickswitch PRIVATE -Wall -Wextra)
    add_test(NAME test_quickswitch COMMAND test_quickswitch)

    # Pure kpidash service-card builder: key shape, payload fields, escaping,
    # and the publish throttle (no Redis/LVGL). The contract is kpidash's and it
    # fails silently, so the shape is pinned here rather than on the board.
    add_executable(test_service_card
        tests/test_service_card.c
        src/service_card.c
    )
    target_include_directories(test_service_card PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_service_card PRIVATE -Wall -Wextra)
    add_test(NAME test_service_card COMMAND test_service_card)

    # The durable panel state file (sprint 039): the whole-file rejection rule,
    # the unknown-key exception that keeps a rollback survivable, and the
    # atomic save — including that a failed save leaves the previous file
    # byte-identical. Pure stdlib, so it runs anywhere ctest does.
    add_executable(test_panel_state tests/test_panel_state.c src/panel_state.c)
    target_include_directories(test_panel_state PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_panel_state PRIVATE -Wall -Wextra)
    add_test(NAME test_panel_state COMMAND test_panel_state)

    # Central-command policy (sprint 039): the `{host}` segment this panel
    # answers to — lowercased, because rpidash2's kernel hostname is `rpiDash2`
    # (korg WI 2277) — and the screenshot path guard, which is the panel's half
    # of CD-23 and is negative-tested against paths outside its own directory.
    add_executable(test_panel_cmd tests/test_panel_cmd.c src/panel_cmd.c)
    target_include_directories(test_panel_cmd PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_panel_cmd PRIVATE -Wall -Wextra)
    add_test(NAME test_panel_cmd COMMAND test_panel_cmd)

    # Endpoint fallbacks, the kvscf-vs-claude auth rule in particular. config.c
    # is stdlib-only, so the test drives the real config_load() through setenv
    # rather than re-stating its rules.
    add_executable(test_config tests/test_config.c src/config.c src/panel_cmd.c)
    target_include_directories(test_config PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_config PRIVATE -Wall -Wextra)
    add_test(NAME test_config COMMAND test_config)

    # The one test here that is not C: what publisher/claude-pub.sh hands to
    # kdash-pub. Since the CD-7 port, one off-contract line makes a whole batch
    # publish nothing, so the format is worth pinning. It stubs kdash-pub, so it
    # needs no network and no installed binary.
    find_program(BASH_EXECUTABLE bash)
    if(BASH_EXECUTABLE)
        add_test(NAME test_publisher_batch
                 COMMAND ${BASH_EXECUTABLE}
                         ${CMAKE_SOURCE_DIR}/publisher/tests/batch-shape.sh)

        # The same, for the Copilot CLI publisher. It drives payloads captured
        # verbatim from a live Copilot session (publisher/tests/fixtures/ghcp/)
        # and also checks the shipped hook template's event names, because a
        # mistyped Copilot event name fires nothing and warns nothing — there
        # is no other place that drift would show up.
        add_test(NAME test_publisher_ghcp
                 COMMAND ${BASH_EXECUTABLE}
                         ${CMAKE_SOURCE_DIR}/publisher/tests/ghcp-batch-shape.sh)
    endif()
endif()
