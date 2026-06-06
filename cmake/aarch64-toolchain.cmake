# CMake toolchain file for cross-compiling to Raspberry Pi 5 (aarch64)
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake ..

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Sysroot containing arm64 headers and libraries synced from the Pi.
# Override with: cmake -DPI5_SYSROOT=/path/to/sysroot ...
if(NOT DEFINED PI5_SYSROOT)
    set(PI5_SYSROOT "$ENV{HOME}/pi5-sysroot")
endif()

set(CMAKE_SYSROOT ${PI5_SYSROOT})
set(CMAKE_FIND_ROOT_PATH ${PI5_SYSROOT})

# Search headers/libs only in the sysroot; run programs (cmake, pkg-config) on the host
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Point pkg-config at the sysroot's .pc files
set(ENV{PKG_CONFIG_PATH} "${PI5_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${PI5_SYSROOT}")
