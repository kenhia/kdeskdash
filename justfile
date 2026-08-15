# KDESKDASH_STORE_URL / KDESKDASH_STORE_HOST live in .env (gitignored) beside
# KVSCF_TOKEN. See docs/deploying.md.
set dotenv-load := true

# List available recipes
default:
    @just --list

# Configure + build the native host tree (unit tests + host binary)
build:
    cmake -B build -DKD_VERSION="$(scripts/version.sh)"
    cmake --build build -j"$(nproc)"

# CI gate: build the host tree and run every unit test
# --no-tests=error: bare ctest prints "No tests were found!!!" and exits 0, so a
# refactor that stopped registering tests would leave this gate green.
check: build
    ctest --test-dir build --output-on-failure --no-tests=error

# Run one test by name, e.g. `just test golz`
test name: build
    ctest --test-dir build -R test_{{ name }} --output-on-failure --no-tests=error

# Cross-compile the aarch64 binary for the Pis (generic aarch64 — one build, every board)
build-pi:
    cmake -B build-pi -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake -DKD_VERSION="$(scripts/version.sh)"
    cmake --build build-pi --target kdeskdash -j"$(nproc)"

# Publish a release to the homelab package store: artifacts/kdeskdash/<version>/
# Refuses a dirty tree; off main it publishes without moving `latest`.
publish:
    scripts/publish.sh

# Install a published version on a Pi (default: the newest published).
# Naming an older version IS the rollback — there is no second verb.
deploy host="rpidash2" version="":
    scripts/deploy.sh deploy ken@{{ host }} {{ version }}

# Dev loop only: cross-compile and push THIS tree to a board, bypassing the
# store. Stamped `-dirty` when the tree is, so the board reports what it is.
# `just publish` + `just deploy` is what ships.
push-dev host="rpidash2": build-pi
    scripts/deploy.sh push-dev ken@{{ host }} build-pi/kdeskdash fonts/ttf/SymbolsNerdFont-Regular.ttf

# What is published, what is cached here, and what each board is running
versions:
    scripts/deploy.sh versions ken@rpidash2 ken@rpidash3

# One-time per device: install the systemd unit (from a published version) +
# that host's env file. Re-run everywhere after changing deploy/kdeskdash.service.
install-service host="rpidash2" version="":
    scripts/deploy.sh install-service ken@{{ host }} deploy/hosts/{{ host }}.env {{ version }}

# One-time / after Pi apt changes: rsync a Pi sysroot to ~/pi-sysroot (one sysroot serves every board)
sync-sysroot host="rpidash2":
    scripts/sync-sysroot.sh {{ host }}

# Headless GoLZ balance sweep (Monte Carlo over the pure core)
golz-mc *ARGS:
    cmake --build build --target golz_mc
    ./build/golz_mc {{ ARGS }}
