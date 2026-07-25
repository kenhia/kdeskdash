# List available recipes
default:
    @just --list

# Configure + build the native host tree (unit tests + host binary)
build:
    cmake -B build
    cmake --build build -j"$(nproc)"

# CI gate: build the host tree and run every unit test
check: build
    ctest --test-dir build --output-on-failure

# Run one test by name, e.g. `just test golz`
test name: build
    ctest --test-dir build -R test_{{ name }} --output-on-failure

# Cross-compile the aarch64 binary for the Pi
build-pi:
    cmake -B build-pi -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake
    cmake --build build-pi --target kdeskdash -j"$(nproc)"

# Cross-compile, scp the binary to ken@rpidash2, restart the service
deploy: build-pi
    cmake --build build-pi --target deploy

# One-time: install the systemd unit + env file on the Pi
install-service: build-pi
    cmake --build build-pi --target install-service

# One-time / after Pi apt changes: rsync the Pi sysroot to ~/pi5-sysroot
sync-sysroot:
    scripts/sync-sysroot.sh

# Headless GoLZ balance sweep (Monte Carlo over the pure core)
golz-mc *ARGS:
    cmake --build build --target golz_mc
    ./build/golz_mc {{ ARGS }}
