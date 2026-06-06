# kdeskdash

A multi-mode, touch-enabled desk dashboard for the Raspberry Pi 5, built with
[LVGL](https://lvgl.io/). It runs fullscreen on an 11.26" 1920x440 capacitive touch
panel and is designed to host multiple interactive *modes* (dev stats, Game of Life, a
main-menu launcher, …). Sibling project to `kpidash`, reusing its LVGL + DRM + Pi-sysroot
cross-compile approach and adding touch input.

> Status: **pre-MVP** — a single screen that draws shapes, text, and an image, and
> responds to touch. The multi-mode framework, gesture navigation, and shared-store
> (Redis) data path are planned but not yet implemented. See
> [docs/plans](docs/plans/) and [docs/brainstorms](docs/brainstorms/).

## Hardware / target

- Raspberry Pi 5 (8GB), Debian 13 (Trixie), hostname `rpidash2`, user `ken`
- GeeekPi 11.26" 1920x440 HDMI capacitive touch (ILITEK controller)
- Display: DRM `/dev/dri/card1` (vc4 GPU) · Touch: evdev `/dev/input/event1`

## Build (cross-compile from a dev host)

### 1. Prerequisites

```bash
# On the dev host: cross toolchain
sudo apt-get install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu cmake pkg-config rsync

# On the Pi: DRM dev headers (linux/input.h for touch is already present)
ssh ken@rpidash2 'sudo apt-get install -y libdrm-dev'
```

### 2. Clone with submodules

```bash
git clone --recurse-submodules <repo-url>   # LVGL is pinned at v9.2.2 in lib/lvgl
cd kdeskdash
```

### 3. Sync the Pi sysroot

```bash
scripts/sync-sysroot.sh        # rsyncs /lib, /usr/lib, /usr/include into ~/pi5-sysroot
```

### 4. Build and deploy

```bash
cmake -B build-pi -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake
cmake --build build-pi --target kdeskdash -j"$(nproc)"
cmake --build build-pi --target deploy        # scp binary to ken@rpidash2:~/kdeskdash
```

## Run (on the Pi)

DRM master requires root for the pre-MVP:

```bash
sudo -E ./kdeskdash      # Ctrl-C to exit
```

| Environment variable | Default | Description |
|----------------------|---------|-------------|
| `KDESKDASH_DRM_DEV`   | `/dev/dri/card1`     | DRM device (card1 = vc4 GPU) |
| `KDESKDASH_TOUCH_DEV` | `/dev/input/event1`  | evdev touch device (ILITEK)  |

## Regenerating the demo image

The image is vendored as an LVGL C array (`assets/brain_rot.c`) so the binary needs no
PNG decoder. To regenerate from `assets/brain_rot.png`:

```bash
python3 -m venv .venv-tools && .venv-tools/bin/pip install pypng pillow lz4
scripts/gen-image.sh
```

## Project layout

```
kdeskdash/
├── CMakeLists.txt                  # LVGL + libdrm + pthread; deploy target
├── lv_conf.h                       # LVGL config: DRM + EVDEV + Montserrat fonts
├── cmake/aarch64-toolchain.cmake   # Pi 5 cross-compile toolchain
├── scripts/
│   ├── sync-sysroot.sh             # rsync Pi sysroot for cross-compilation
│   └── gen-image.sh                # regenerate the LVGL C-array image
├── assets/                         # source PNG + generated brain_rot.c
├── src/
│   ├── main.c                      # entry: DRM + evdev bring-up, main loop, teardown
│   ├── config.{c,h}                # env-var configuration
│   └── demo_screen.{c,h}           # pre-MVP shapes/text/image/touch demo
├── lib/lvgl/                       # LVGL v9.2.2 (submodule)
└── docs/                           # brainstorms, plans, solutions
```
