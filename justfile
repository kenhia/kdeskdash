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

# Cross-compile the aarch64 binary for the Pis (generic aarch64 — one build, every board)
build-pi:
    cmake -B build-pi -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake
    cmake --build build-pi --target kdeskdash -j"$(nproc)"

# Cross-compile, scp the binary to a Pi, restart the service (e.g. `just deploy rpidash3`)
deploy host="rpidash2": build-pi
    scripts/deploy.sh deploy ken@{{ host }} build-pi/kdeskdash fonts/ttf/SymbolsNerdFont-Regular.ttf

# One-time per device: install the systemd unit + that host's env file
install-service host="rpidash2":
    scripts/deploy.sh install-service ken@{{ host }} deploy/kdeskdash.service deploy/hosts/{{ host }}.env

# One-time / after Pi apt changes: rsync a Pi sysroot to ~/pi-sysroot (one sysroot serves every board)
sync-sysroot host="rpidash2":
    scripts/sync-sysroot.sh {{ host }}

# Headless GoLZ balance sweep (Monte Carlo over the pure core)
golz-mc *ARGS:
    cmake --build build --target golz_mc
    ./build/golz_mc {{ ARGS }}
