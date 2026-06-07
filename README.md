# kdeskdash

A multi-mode, touch-enabled desk dashboard for the Raspberry Pi 5, built with
[LVGL](https://lvgl.io/). It runs fullscreen on an 11.26" 1920x440 capacitive touch
panel and is designed to host multiple interactive *modes* (dev stats, Game of Life, a
main-menu launcher, …). Sibling project to `kpidash`, reusing its LVGL + DRM + Pi-sysroot
cross-compile approach and adding touch input.

> Status: **MVP** — a multi-mode shell with swipe navigation (Game of Life, Clock, and a
> Menu launcher), optional Redis remote control / last-mode persistence / Game of Life
> settings injection, and a systemd service for boot-to-dashboard. See
> [docs/plans](docs/plans/) and [docs/brainstorms](docs/brainstorms/).

## Modes

- **Menu** — swipe-down launcher with a tile per content mode; tap to open. Startup default.
- **Game of Life** — full-screen Conway's Game of Life; settings randomize per entry (or are
  injected via Redis).
- **Clock** — local (America/Los_Angeles) + UTC time and a wall-clock stopwatch.

Navigation: swipe **left/right** to cycle content modes, swipe **down** for the Menu.

## Hardware / target

- Raspberry Pi 5 (8GB), Debian 13 (Trixie), hostname `rpidash2`, user `ken`
- GeeekPi 11.26" 1920x440 HDMI capacitive touch (ILITEK controller)
- Display: DRM `/dev/dri/card1` (vc4 GPU) · Touch: evdev `/dev/input/by-id/usb-ILITEK_ILITEK-TOUCH-event-if00`
- 3D Printed case (work in progress, will include STLs once I finish the design)

## Build (cross-compile from a dev host)

### 1. Prerequisites

```bash
# On the dev host: cross toolchain
sudo apt-get install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu cmake pkg-config rsync

# On the Pi: DRM dev headers + hiredis (linux/input.h for touch is already present)
ssh ken@rpidash2 'sudo apt-get install -y libdrm-dev libhiredis-dev'
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

DRM master and evdev require root.

```bash
sudo -E ./kdeskdash      # Ctrl-C to exit
```

| Environment variable | Default | Description |
|----------------------|---------|-------------|
| `KDESKDASH_DRM_DEV`    | `/dev/dri/card1`     | DRM device (card1 = vc4 GPU) |
| `KDESKDASH_ROTATE_180` | _(off)_              | Flip the whole display 180° (panel mounts inverted); `1`/`true`/`yes`/`on` to enable |
| `KDESKDASH_TOUCH_DEV`  | `/dev/input/by-id/usb-ILITEK_ILITEK-TOUCH-event-if00` | evdev touch device; the by-id symlink is stable across replug/reboot |
| `KDESKDASH_REDIS_HOST` | `127.0.0.1`          | Redis host (optional)        |
| `KDESKDASH_REDIS_PORT` | `6379`               | Redis port                   |
| `REDISCLI_AUTH`        | _(unset)_            | Redis password, if any (AUTH)|

## Redis (optional)

Redis enables remote control, last-mode persistence, and Game of Life settings injection.
The dashboard runs fully by touch without it.

```bash
ssh ken@rpidash2 'sudo apt-get install -y redis-server'   # enabled on install
```

Keys:

| Key | Type | Purpose |
|-----|------|---------|
| `kdeskdash:active_mode`  | string | Active mode id; `SET` to switch remotely, written on every change (persistence). |
| `kdeskdash:gol:settings` | hash   | One-shot Game of Life settings, consumed (deleted) on the next GoL entry. |

Examples (run on the Pi or any host pointed at its Redis):

```bash
redis-cli set kdeskdash:active_mode clock         # switch to the Clock mode
redis-cli hset kdeskdash:gol:settings \
  cell_size 6 padding 1 density 0.4 trail 1 trail_turns 8 speed_ms 120
redis-cli set kdeskdash:active_mode game_of_life  # applies the injected settings
```

GoL fields (all optional; absent fields randomize): `cell_size` (1–64), `padding` (0–16),
`density` (0–1.0), `trail` (0/1), `trail_turns` (1–64), `speed_ms` (10–5000).

## Service (boot-to-dashboard)

Install the systemd unit once, then deploys restart it automatically:

```bash
cmake --build build-pi --target install-service   # installs unit + /etc/kdeskdash/kdeskdash.env, enables
ssh ken@rpidash2 'sudo systemctl start kdeskdash'
```

Edit `/etc/kdeskdash/kdeskdash.env` on the Pi to override the environment variables above
(template: [deploy/kdeskdash.env.example](deploy/kdeskdash.env.example)). The deploy target
(`cmake --build build-pi --target deploy`) stops the service, installs the binary to
`/usr/local/bin/kdeskdash`, and starts it.

## Project layout

```
kdeskdash/
├── CMakeLists.txt                  # LVGL + libdrm + hiredis + pthread; deploy/install-service
├── lv_conf.h                       # LVGL config: DRM + EVDEV + Montserrat fonts
├── cmake/aarch64-toolchain.cmake   # Pi 5 cross-compile toolchain
├── deploy/
│   ├── kdeskdash.service           # systemd unit (boot-to-dashboard)
│   └── kdeskdash.env.example       # env template -> /etc/kdeskdash/kdeskdash.env
├── scripts/
│   ├── sync-sysroot.sh             # rsync Pi sysroot for cross-compilation
│   └── deploy.sh                   # remote deploy / systemd install
├── src/
│   ├── main.c                      # entry: DRM + evdev bring-up, main loop, teardown
│   ├── config.{c,h}                # env-var configuration
│   ├── shell.{c,h}                 # mode shell: registration, gestures, lifecycle
│   ├── redis.{c,h}                 # optional Redis client (control/persistence/injection)
│   ├── gol.{c,h} / stopwatch.{c,h} # pure, host-tested mode cores
│   └── modes/                      # game_of_life, clock, menu
├── tests/                          # host unit tests (registry, gol, stopwatch)
├── lib/lvgl/                       # LVGL v9.2.2 (submodule)
└── docs/                           # brainstorms, plans, solutions
```
