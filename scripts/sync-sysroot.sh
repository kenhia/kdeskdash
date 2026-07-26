#!/usr/bin/env bash
# Sync an aarch64 sysroot from a Pi for cross-compilation.
#
# Usage:
#   scripts/sync-sysroot.sh [host]        # host defaults to rpidash2
#
# One sysroot serves every dashboard Pi: rpidash2 (Pi 5) and rpidash3 (Pi 4) run
# the same Debian 13 Trixie aarch64 userspace, and the binary we build is generic
# aarch64. Sync from whichever board is handy; the sysroot is not board-specific.
#
# Prerequisite on the Pi: sudo apt-get install -y libdrm-dev libhiredis-dev
# (linux/input.h, needed by lv_evdev, ships with linux-libc-dev which is already present.)
set -euo pipefail

# Positional host wins; PI_HOST stays supported for existing habits. A bare
# hostname gets the ken@ user prefix the fleet uses.
host="${1:-${PI_HOST:-rpidash2}}"
case "$host" in
  *@*) PI_HOST="$host" ;;
  *)   PI_HOST="ken@$host" ;;
esac

# PI5_SYSROOT is the pre-multi-Pi name, still honoured; so is an existing
# ~/pi5-sysroot, so nobody has to re-sync just because the name changed.
if [ -n "${PI_SYSROOT:-}" ]; then
  SYSROOT="$PI_SYSROOT"
elif [ -n "${PI5_SYSROOT:-}" ]; then
  SYSROOT="$PI5_SYSROOT"
elif [ ! -d "$HOME/pi-sysroot" ] && [ -d "$HOME/pi5-sysroot" ]; then
  SYSROOT="$HOME/pi5-sysroot"
else
  SYSROOT="$HOME/pi-sysroot"
fi

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

echo "Verifying hiredis headers/pkgconfig are present:"
ls "$SYSROOT/usr/include/hiredis/hiredis.h" \
   "$SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig/hiredis.pc" 2>/dev/null \
   && echo "OK: hiredis dev files found in sysroot" \
   || echo "WARNING: hiredis dev files missing — run 'sudo apt-get install libhiredis-dev' on the Pi and re-sync"
