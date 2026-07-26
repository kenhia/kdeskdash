# CMake toolchain file for cross-compiling to the Raspberry Pi dashboards (aarch64)
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake ..
#
# The output is generic aarch64 — one build tree serves both the Pi 5 (rpidash2)
# and the Pi 4 (rpidash3). They run the same Debian 13 Trixie userspace, so one
# sysroot serves both too; only the kernel flavor differs, which does not reach
# the linker.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Sysroot containing arm64 headers and libraries synced from a Pi.
# Override with: cmake -DPI_SYSROOT=/path/to/sysroot ...
# PI5_SYSROOT is the pre-multi-Pi name, still honoured so existing build trees
# and shell profiles keep working.
if(NOT DEFINED PI_SYSROOT)
    if(DEFINED PI5_SYSROOT)
        set(PI_SYSROOT "${PI5_SYSROOT}")
    elseif(EXISTS "$ENV{HOME}/pi-sysroot")
        set(PI_SYSROOT "$ENV{HOME}/pi-sysroot")
    elseif(EXISTS "$ENV{HOME}/pi5-sysroot")
        # Legacy location from before the sysroot was renamed; use it rather
        # than forcing a re-sync.
        set(PI_SYSROOT "$ENV{HOME}/pi5-sysroot")
    else()
        set(PI_SYSROOT "$ENV{HOME}/pi-sysroot")
    endif()
endif()

set(CMAKE_SYSROOT ${PI_SYSROOT})
set(CMAKE_FIND_ROOT_PATH ${PI_SYSROOT})

# Search headers/libs only in the sysroot; run programs (cmake, pkg-config) on the host
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Point pkg-config at the sysroot's .pc files
set(ENV{PKG_CONFIG_PATH} "${PI_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${PI_SYSROOT}")
