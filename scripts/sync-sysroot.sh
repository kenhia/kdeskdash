#!/usr/bin/env bash
# Sync an aarch64 sysroot from the Pi for cross-compilation.
#
# Prerequisite on the Pi: sudo apt-get install -y libdrm-dev
# (linux/input.h, needed by lv_evdev, ships with linux-libc-dev which is already present.)
set -euo pipefail

PI_HOST="${PI_HOST:-ken@rpidash2}"
SYSROOT="${PI5_SYSROOT:-$HOME/pi5-sysroot}"

mkdir -p "$SYSROOT/lib" "$SYSROOT/usr"

echo "Syncing sysroot from $PI_HOST into $SYSROOT ..."
rsync -az --rsync-path="sudo rsync" "$PI_HOST:/lib/aarch64-linux-gnu" "$SYSROOT/lib/"
rsync -az --rsync-path="sudo rsync" "$PI_HOST:/usr/lib/aarch64-linux-gnu" "$SYSROOT/usr/lib/"
rsync -az "$PI_HOST:/usr/include" "$SYSROOT/usr/"

echo "Done. Verifying DRM headers/pkgconfig are present:"
ls "$SYSROOT/usr/include/xf86drm.h" \
   "$SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig/libdrm.pc" 2>/dev/null \
   && echo "OK: libdrm dev files found in sysroot" \
   || echo "WARNING: libdrm dev files missing — run 'sudo apt-get install libdrm-dev' on the Pi and re-sync"
