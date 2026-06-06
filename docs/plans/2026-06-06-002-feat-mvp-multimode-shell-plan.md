---
title: "feat: MVP multi-mode shell with Game of Life, Clock, Redis, and auto-start"
type: feat
status: active
date: 2026-06-06
origin: docs/brainstorms/2026-06-06-mvp-shell-modes-redis-requirements.md
---

# feat: MVP multi-mode shell with Game of Life, Clock, Redis, and auto-start

## Overview

Turn the pre-MVP display/touch skeleton into a usable desk appliance: a mode shell that
hosts multiple full-screen modes, swipe navigation between them, a Menu launcher, two real
modes (Game of Life and Clock), a Redis data/control path for remote mode-switching and
Game of Life settings injection, and a systemd service so it auto-starts on boot. Builds on
the committed pre-MVP (`src/main.c`, `src/demo_screen.c`, DRM display + evdev touch).

## Problem Frame

The pre-MVP proved display + touch on `rpidash2` but only renders a static demo screen. The
MVP makes it something Ken gets daily use from — boots on its own, shows a Clock, can run an
ambient Game of Life, and is driven entirely by touch with optional remote control via Redis
(see origin: docs/brainstorms/2026-06-06-mvp-shell-modes-redis-requirements.md). It also
lands the durable multi-mode architecture (registry + lifecycle + navigation) so later modes
and clients drop in without re-churning the core.

## Requirements Trace

Mapping to origin requirements R1–R20:

- R1–R6 (Mode shell & navigation): Units 1, 5
- R7–R11 (Game of Life): Unit 2
- R12–R15 (Clock): Unit 3
- R16–R18 (Redis data/control): Unit 5
- R19–R20 (Deployment & runtime): Unit 6
- R5 Menu launcher: Unit 4

## Scope Boundaries

- No client apps, telemetry, or dev/usage-graph mode (origin scope boundary).
- No on-device settings UI — Game of Life settings come from Redis or randomization.
- Stopwatch state does not persist across an app/Pi restart (only active mode persists).
- Single fixed green for Game of Life; multi-color deferred.
- No on-screen mode indicator / swipe hint.
- No screen blanking, brightness, or DPMS scheduling.

## Context & Research

### Relevant Code and Patterns

- `src/main.c` — current entry point: config → `lv_init` → `lv_linux_drm_create` →
  `demo_screen_create()` → `lv_evdev_create(LV_INDEV_TYPE_POINTER, ...)` → main loop
  (`lv_timer_handler()` + `usleep`). The shell replaces `demo_screen_create()` and adds a
  per-loop `tick` hook plus a Redis poll.
- `src/config.{c,h}` — `kdeskdash_config_t {drm_dev, touch_dev}` with `KDESKDASH_*` env
  overrides. Extend with Redis host/port/auth.
- `src/demo_screen.{c,h}` — existing LVGL screen-building patterns (rounded rects, labels,
  buttons with `lv_obj_add_event_cb(..., LV_EVENT_CLICKED, ...)`, Montserrat fonts). Modes
  reuse these idioms. `demo_screen` is removed once the Menu/modes exist.
- `CMakeLists.txt` — LVGL submodule + libdrm + pthread; `LV_LVGL_H_INCLUDE_SIMPLE`,
  `LV_CONF_PATH`; `deploy` target scps to `ken@rpidash2`. Extend with hiredis and a host-only
  test target.
- `lv_conf.h` — `LV_COLOR_DEPTH 32`, DRM + EVDEV on, CANVAS widget on, Montserrat 14/20/28/36.
  Add a large font for the Clock and confirm `LV_USE_CANVAS 1`.

### External / Library Findings (LVGL v9.2.2, vendored in `lib/lvgl/`)

- Gestures: `LV_EVENT_GESTURE` (`lib/lvgl/src/misc/lv_event.h`); read direction with
  `lv_indev_get_gesture_dir(lv_indev_active())` (`lib/lvgl/src/indev/lv_indev.h`) returning
  `LV_DIR_LEFT/RIGHT/TOP/BOTTOM` (`lib/lvgl/src/misc/lv_area.h`). Gesture fires on the active
  object and bubbles when the object has `LV_OBJ_FLAG_GESTURE_BUBBLE`. Gestures and
  `LV_EVENT_CLICKED` are mutually exclusive (a movement past the gesture limit suppresses the
  click). Default threshold `LV_INDEV_DEF_GESTURE_LIMIT = 50` px, velocity `3` px/frame.
- Canvas: `lv_canvas_create` / `lv_canvas_set_buffer(obj, buf, w, h, cf)` with
  `LV_COLOR_FORMAT_XRGB8888`; buffer sized via `LV_CANVAS_BUF_SIZE`; `lv_canvas_set_px`,
  `lv_canvas_fill_bg`, and layer API (`lv_canvas_init_layer`/`finish_layer`). The canvas owns
  a contiguous XRGB8888 buffer we can write as `uint32_t*` directly for speed. `LV_USE_CANVAS`
  defaults on.
- Redis pattern (from sibling `kpidash`, `src/redis.c`): `redisConnectWithTimeout` with a 1s
  connect timeout and 50ms read timeout, `REDISCLI_AUTH` env → `AUTH`, static
  `redisContext *g_ctx` with a `reconnect_if_needed()` guard, synchronous polling in the LVGL
  main loop (no pub/sub, no extra thread). hiredis linked via `find_package(hiredis CONFIG)`
  with a `pkg_check_modules` fallback. `kpidash` has no `.service` in-repo; its deploy uses
  `ssh ... sudo systemctl stop/start kpidash`.

### Institutional Learnings

- `docs/solutions/` is currently empty; the pre-MVP plan
  (`docs/plans/2026-06-06-001-feat-premvp-display-touch-plan.md`) records that the panel
  reports native 1920x440, touch needs no calibration/axis-swap, and clean SIGTERM teardown
  works. Fish shell does not support bash `$(...)`; use literal values (e.g. `-j8`).
- Cross-compile uses `cmake/aarch64-toolchain.cmake` (`PI5_SYSROOT`, pkg-config pointed at
  sysroot, `-Wl,--allow-shlib-undefined` for glibc skew). `libhiredis-dev` must be installed
  on `rpidash2` and synced into the sysroot, mirroring the earlier `libdrm-dev` step.

## Key Technical Decisions

- **Mode abstraction**: a `mode_t` with `id`, `title`, an LVGL `screen`, and
  `activate`/`deactivate`/`tick` function pointers plus a `void *state`. A registry holds the
  ordered content modes (Game of Life, Clock); the Menu is a registered mode reached only by
  swipe-down. Rationale: minimal boilerplate to add a mode (R1), clean lifecycle so hidden
  modes do no work (R2).
- **Navigation via gestures on each mode screen**: the shell attaches an `LV_EVENT_GESTURE`
  handler to every mode's screen; interactive children carry `LV_OBJ_FLAG_GESTURE_BUBBLE` so
  swipes that begin on a button still navigate while taps still click. LEFT/RIGHT cycle
  content modes (wrapping), BOTTOM (down) returns to Menu. Exact `LV_DIR_*`→intent mapping
  verified on hardware. Rationale: resolves the swipe-vs-tap conflict using LVGL's built-in
  gesture/click exclusivity (R3, R4).
- **Game of Life renders via a single full-screen `lv_canvas`** in XRGB8888, writing the
  owned buffer directly as `uint32_t*` and invalidating once per generation (every `speed`
  ms), not per frame. Rationale: at cell-size 1 the grid is ~845k cells; direct buffer writes
  + one invalidate per generation is the only approach that stays cheap (R7).
- **Pure, host-testable core logic**: `gol_step()` (toroidal next-generation), registry
  next/prev wrap, and stopwatch elapsed→display formatting are pure functions in files with no
  LVGL/DRM dependency, covered by a host-only `ctest` target. Rationale: gives real coverage
  for the toroidal-wrap correctness risk without a GUI test harness.
- **Redis schema** (no cJSON; use native types):
  - `kdeskdash:active_mode` — string mode id. The shell SETs it on every mode change (this is
    both remote-visible state and the persisted "last mode"), GETs it on startup to restore
    (R6), and polls it each cycle; if it differs from the current mode and names a valid mode,
    the shell switches (R16).
  - `kdeskdash:gol:settings` — a hash (`HGETALL`) with fields `cell_size`, `padding`,
    `density`, `trail`, `trail_turns`, `speed_ms`. On Game of Life activation the shell reads
    it, applies present fields, `DEL`s the key, and randomizes any absent field (R9, R17).

  Rationale: a hash avoids JSON parsing; one string key unifies remote control + persistence.
- **Redis is optional at runtime**: if unavailable, the app still runs (touch nav only, no
  persistence, Game of Life fully randomizes); the shell attempts reconnect each poll,
  mirroring kpidash. Rationale: the dashboard must never be bricked by a Redis outage.
- **Connection config via env**, `KDESKDASH_REDIS_HOST` (default `127.0.0.1`),
  `KDESKDASH_REDIS_PORT` (default `6379`), `REDISCLI_AUTH` (auth, standard redis tooling var),
  mirroring kpidash (R18).
- **systemd service runs as root** (DRM `/dev/dri/card1` + evdev `/dev/input/event1` both need
  privilege), `After`/`Wants redis-server.service`, `Restart=always`, env via
  `EnvironmentFile=/etc/kdeskdash/kdeskdash.env`, `ExecStart=/home/ken/kdeskdash` to match the
  existing scp deploy path. Rationale: simplest reliable appliance boot (R19); a dedicated
  `video`+`input` group user is a future hardening step.

## Open Questions

### Resolved During Planning

- Rendering approach for Game of Life: single `lv_canvas`, direct buffer writes, one
  invalidate per generation.
- Swipe-vs-tap conflict: LVGL gesture/click exclusivity + `LV_OBJ_FLAG_GESTURE_BUBBLE`.
- GoL settings transport: Redis hash, no cJSON dependency.
- Remote control + last-mode persistence: unified on `kdeskdash:active_mode`.
- Service privilege: run as root.

### Deferred to Implementation

- Exact `LV_DIR_*`→navigation mapping (which physical swipe direction LVGL reports) — confirm
  on hardware and adjust.
- Game of Life performance envelope on the real panel: lowest practical `cell_size`, the
  `speed_ms` "sweet spot", and whether a minimum cell size must be enforced — tune on device.
- Final randomization ranges/formulas (origin R10 values are starting points: cell-size 1–10,
  padding bounded by cell size, density range, ~33% chance of no trail, speed 0–2000ms).
- Trail representation: an age buffer (`uint8` per cell) scaling green brightness by
  `age/trail_turns` — confirm visual quality and memory cost during implementation.
- Whether the Redis poll cadence should differ from the 1s kpidash default for snappier
  remote mode switches.

## High-Level Technical Design

> *This illustrates the intended approach and is directional guidance for review, not
> implementation specification. The implementing agent should treat it as context, not code to
> reproduce.*

```text
main loop (per iteration):
  redis_poll()            # reconnect-if-needed; read kdeskdash:active_mode;
                          #   if changed+valid -> shell_set_active(mode)
  shell_tick()            # active_mode->tick(active_mode)   (e.g. GoL step on speed timer)
  lv_timer_handler()
  usleep(...)

mode_t { id; title; screen; activate(); deactivate(); tick(); state; }

shell:
  registry[] = { game_of_life, clock }   # content modes, ordered
  menu                                    # reached via swipe-down
  active
  shell_set_active(m): old->deactivate(); load m->screen; m->activate();
                       SET kdeskdash:active_mode = m->id
  gesture(dir): LEFT/RIGHT -> set_active(next/prev wrapped); BOTTOM -> set_active(menu)
  startup: GET kdeskdash:active_mode -> restore, else menu
```

## Implementation Units

- [x] **Unit 1: Mode shell, registry, and swipe navigation**

**Goal:** Replace the static demo screen with a mode shell that owns the active mode,
per-mode `activate`/`deactivate`/`tick` lifecycle, and gesture-driven navigation. Register
two temporary placeholder modes to prove swiping and lifecycle before the real modes land.

**Requirements:** R1, R2, R3, R4

**Dependencies:** None (builds on existing `main.c`/LVGL setup)

**Files:**
- Create: `src/mode.h` (the `mode_t` struct and registry API)
- Create: `src/shell.c`, `src/shell.h` (registry storage, `shell_set_active`, gesture handler,
  `shell_tick`, startup default)
- Modify: `src/main.c` (call `shell_init`/`shell_tick`; remove `demo_screen_create` wiring)
- Modify: `CMakeLists.txt` (add `src/shell.c`)
- Create: `tests/test_registry.c` (host-only next/prev wrap)
- Modify: `CMakeLists.txt` (host-only `ctest` target — see Unit 2 too)

**Approach:**
- `mode_t` holds an LVGL `screen` (created lazily on first activate or at registration) and
  the three lifecycle hooks. `shell_set_active(m)` calls the old mode's `deactivate`, loads
  `m->screen` via `lv_screen_load`, then calls `m->activate`.
- Attach the `LV_EVENT_GESTURE` handler to each mode's screen; in the callback read
  `lv_indev_get_gesture_dir(lv_indev_active())` and route LEFT/RIGHT→next/prev content mode
  (wrapping), BOTTOM→Menu (placeholder/first mode until Unit 4).
- Add `LV_OBJ_FLAG_GESTURE_BUBBLE` to interactive children so swipes started on widgets still
  navigate.
- Keep mode registration declarative: an array the shell iterates; adding a mode is one entry.

**Execution note:** Manual hardware verification for the LVGL/touch path (no GUI test harness),
mirroring the pre-MVP. Pure registry wrap logic is covered by a host test.

**Patterns to follow:** existing screen/event-callback idioms in `src/demo_screen.c`; main-loop
structure in `src/main.c`.

**Test scenarios:**
- Happy path (host): `registry_next` from last content mode wraps to first; `registry_prev`
  from first wraps to last; with N=2 modes, next and prev both yield the other mode.
- Edge case (host): single-mode registry — next/prev returns the same mode.
- Happy path (hardware): swipe left/right cycles placeholder modes; swipe down lands on the
  swipe-down target; a tap on a placeholder button still fires CLICKED, a swipe over it still
  navigates.

**Verification:** On device, all three placeholder transitions work by touch and a deactivated
placeholder stops updating; `ctest` registry test passes on the host.

---

- [ ] **Unit 2: Game of Life mode**

**Goal:** A full-screen Conway's Game of Life mode with toroidal wrap, canvas rendering, the
configurable settings (randomized within ranges at activation), and the optional fade trail.

**Requirements:** R7, R8, R9, R10, R11

**Dependencies:** Unit 1 (mode shell)

**Files:**
- Create: `src/modes/game_of_life.c`, `src/modes/game_of_life.h` (mode + LVGL canvas glue)
- Create: `src/gol.c`, `src/gol.h` (pure grid: `gol_step` toroidal, random seed to density,
  trail age update — no LVGL include)
- Create: `tests/test_gol.c` (host-only)
- Modify: `CMakeLists.txt` (sources + host test)
- Modify: `lv_conf.h` only if a canvas/feature flag proves missing (CANVAS already on)

**Approach:**
- Pure core in `gol.c`: a cell grid plus a `uint8` age buffer; `gol_step` computes the next
  generation with toroidal neighbor indexing; a seed function fills generation 0 to `density`.
  Trail: when enabled, a dying cell's age decays over `trail_turns`, scaling brightness.
- Rendering in `game_of_life.c`: one full-screen `lv_canvas` in `LV_COLOR_FORMAT_XRGB8888`
  with a statically/heap-allocated buffer (`LV_CANVAS_BUF_SIZE`). On each generation, write
  the owned buffer directly as `uint32_t*` — fill each cell's `cell_size` block in green (or
  age-scaled green for trails), leaving `padding` as background — then `lv_obj_invalidate`
  once.
- `activate`: read settings (applied by the shell from Redis in Unit 5; until then,
  randomize), allocate buffers for the resulting grid dimensions, seed generation 0.
  `deactivate`: free buffers / stop stepping. `tick`: advance one generation when `speed_ms`
  has elapsed since the last.
- Grid dimensions derive from `cell_size`+`padding` over 1920x440.

**Execution note:** Implement `gol.c` test-first against `tests/test_gol.c` — the toroidal
wrap is the flagged correctness risk.

**Patterns to follow:** canvas usage per `lib/lvgl/examples/widgets/canvas/`; mode lifecycle
from Unit 1.

**Test scenarios:**
- Happy path (host): a blinker oscillates with period 2; a 2x2 block is a still life.
- Edge case (host, toroidal): a live cell at corner (0,0) counts neighbors at the opposite
  edges/corner; a glider crossing an edge re-enters the opposite side; an all-dead grid stays
  all-dead.
- Edge case (host): seeding at density 0.0 yields no live cells; density 1.0 yields all live;
  ~0.33 yields roughly a third (within tolerance).
- Edge case (host, trail): with trail off a dead cell is immediately background; with trail on
  and `trail_turns=N`, a just-died cell decays to background over N steps.
- Happy path (hardware): mode fills the panel, animates at the chosen speed, wraps at edges,
  and trail fade is visible when enabled.

**Verification:** `ctest` GoL tests pass; on device the board renders full-screen, wraps, and
animates without visible stutter at a reasonable cell size.

---

- [ ] **Unit 3: Clock mode**

**Goal:** A Clock mode showing large centered local time, UTC on the left, and a
wallclock-based stopwatch on the right with start/stop and reset, to 0.1s resolution.

**Requirements:** R12, R13, R14, R15

**Dependencies:** Unit 1 (mode shell)

**Files:**
- Create: `src/modes/clock.c`, `src/modes/clock.h`
- Create: `src/stopwatch.c`, `src/stopwatch.h` (pure: state + elapsed→formatted string)
- Create: `tests/test_stopwatch.c` (host-only)
- Modify: `CMakeLists.txt` (sources + host test)
- Modify: `lv_conf.h` (enable a large Montserrat font, e.g. `LV_FONT_MONTSERRAT_48`, for the
  centered local time)

**Approach:**
- Three LVGL labels: local time (large, centered), UTC time (left), stopwatch (right) with two
  buttons (Start/Stop toggle, Reset). Buttons carry `LV_OBJ_FLAG_GESTURE_BUBBLE`.
- Time strings from `localtime`/`gmtime` + `strftime`, refreshed in `tick` while active.
- Stopwatch is pure and wallclock-based: it stores `running`, a `start` monotonic timestamp,
  and accumulated elapsed; `stopwatch_elapsed(now)` computes total. Because elapsed derives
  from the wall clock, it stays correct while the mode is hidden; only the on-screen readout
  updates in `tick` while active (R15). Format to 0.1s (e.g. `M:SS.s`).
- `deactivate` simply stops refreshing labels; the stopwatch keeps its running state.

**Execution note:** Implement `stopwatch.c` test-first.

**Patterns to follow:** button + `LV_EVENT_CLICKED` handlers and label styling in
`src/demo_screen.c`.

**Test scenarios:**
- Happy path (host): start at t=0, query at t=12.34s → `0:12.3`; stop freezes elapsed; reset
  returns to `0:00.0`.
- Edge case (host): start/stop/start accumulates across pauses; querying while stopped returns
  the frozen value regardless of `now`.
- Edge case (host): formatting boundaries — 59.9s → `0:59.9`, 60.0s → `1:00.0`, sub-0.1s
  rounding is stable.
- Integration (hardware): start the stopwatch, swipe to Game of Life and back; elapsed
  reflects the wall-clock time that passed while hidden, and the display resumes updating.
- Happy path (hardware): local time centered/large, UTC on the left, both correct.

**Verification:** `ctest` stopwatch tests pass; on device times are correct and the stopwatch
survives mode switches.

---

- [ ] **Unit 4: Menu launcher mode**

**Goal:** A Menu mode that is itself a registered mode and acts as a launcher: a tile per
content mode that opens that mode on tap. Becomes the swipe-down target.

**Requirements:** R5

**Dependencies:** Units 1, 2, 3 (modes exist to launch)

**Files:**
- Create: `src/modes/menu.c`, `src/modes/menu.h`
- Modify: `src/shell.c` (register Menu; route swipe-down to Menu; startup default = Menu)
- Modify: `CMakeLists.txt` (source)

**Approach:**
- Build a tile/grid layout (LVGL flex/grid) with one tappable tile per content mode, labeled
  from each mode's `title`. Tapping a tile calls `shell_set_active(thatMode)`.
- Replace the Unit 1 placeholder swipe-down target with the Menu. Within the Menu, left/right
  swipes are no-ops for the MVP (only tiles navigate out).

**Execution note:** Manual hardware verification (LVGL/touch).

**Patterns to follow:** button/event idioms from `src/demo_screen.c`; mode lifecycle from
Unit 1.

**Test scenarios:**
- Happy path (hardware): swipe down from any content mode opens the Menu; tapping the Game of
  Life tile opens Game of Life; tapping the Clock tile opens Clock.
- Edge case (hardware): tiles reflect exactly the registered content modes (adding a mode adds
  a tile with no menu-specific code change).

**Verification:** On device, the Menu lists both modes and each tile launches the correct mode;
swipe-down consistently returns to the Menu.

---

- [ ] **Unit 5: Redis integration (remote control, settings injection, persistence)**

**Goal:** Add the optional Redis client: connection from env, per-loop poll that drives the
active mode from `kdeskdash:active_mode`, last-mode restore on startup, and Game of Life
settings injection via `kdeskdash:gol:settings`.

**Requirements:** R6, R16, R17, R18

**Dependencies:** Unit 1 (active-mode concept), Unit 2 (GoL settings), Units 3–4 (valid mode
ids to switch to)

**Files:**
- Create: `src/redis.c`, `src/redis.h` (connect/reconnect, `redis_poll`, get/set active mode,
  read+clear GoL settings hash)
- Modify: `src/config.c`, `src/config.h` (add `redis_host`, `redis_port`, `redis_auth` from
  `KDESKDASH_REDIS_HOST`/`KDESKDASH_REDIS_PORT`/`REDISCLI_AUTH`)
- Modify: `src/shell.c` (on `shell_set_active`, `SET kdeskdash:active_mode`; on startup, `GET`
  to restore; expose a hook so GoL activation pulls + clears settings)
- Modify: `src/modes/game_of_life.c` (apply injected settings on activate, falling back to
  randomization for absent fields)
- Modify: `src/main.c` (call `redis_poll` in the loop)
- Modify: `CMakeLists.txt` (hiredis via `find_package(hiredis CONFIG)` + `pkg_check_modules`
  fallback, mirroring kpidash)

**Approach:**
- Mirror kpidash `src/redis.c`: 1s connect timeout, 50ms read timeout, `REDISCLI_AUTH`→`AUTH`,
  static `redisContext *g_ctx`, `reconnect_if_needed()` before each poll. All synchronous in
  the main loop, no extra thread.
- `redis_poll`: reconnect if needed; `GET kdeskdash:active_mode`; if it names a valid mode
  different from the current one, call `shell_set_active`. Failures are swallowed (Redis is
  optional); persistence and remote control simply no-op while disconnected.
- Settings: `HGETALL kdeskdash:gol:settings` on GoL activation, apply present fields, then
  `DEL`; absent fields randomize.
- Persistence (R6): startup `GET kdeskdash:active_mode` restores the last mode, else Menu;
  every `shell_set_active` writes the key.

**Execution note:** Connection/poll behavior is integration-verified against a real Redis on
the Pi; keep any field-parsing helper small enough to unit-test if it grows non-trivial.

**Patterns to follow:** `kpidash/src/redis.c` connection/reconnect/poll structure;
`kpidash/CMakeLists.txt` hiredis linkage; `kpidash/src/config.c` env parsing.

**Test scenarios:**
- Happy path (hardware + redis-cli): `SET kdeskdash:active_mode clock` switches the on-screen
  mode within one poll; swiping locally updates the key (observable via `GET`).
- Happy path (hardware + redis-cli): `HSET kdeskdash:gol:settings cell_size 4 speed_ms 120 ...`
  then entering Game of Life uses those values; the key is gone afterward (`EXISTS` → 0);
  re-entering randomizes.
- Edge case (hardware): with Redis stopped, the app still boots and navigates by touch; Game
  of Life randomizes; on Redis return the next poll resumes remote control.
- Edge case (hardware): an invalid/unknown mode id in `kdeskdash:active_mode` is ignored (no
  crash, current mode retained).
- Integration (hardware): after a restart with Redis up, the app reopens the last active mode.

**Verification:** Remote `redis-cli` mode switches and settings injection work on device;
killing Redis does not break the dashboard; last mode is restored across restart.

---

- [ ] **Unit 6: systemd service, Redis on the Pi, deploy, and docs**

**Goal:** Make the dashboard a boot appliance: install/enable `redis-server` and a
`kdeskdash.service` on `rpidash2`, wire the deploy target to manage the service, and document
the setup. Ensure the cross-build links hiredis from the sysroot.

**Requirements:** R19, R20

**Dependencies:** Unit 5 (Redis dependency, env config)

**Files:**
- Create: `deploy/kdeskdash.service` (committed unit file)
- Create: `deploy/kdeskdash.env.example` (env template: `KDESKDASH_REDIS_HOST`,
  `KDESKDASH_REDIS_PORT`, `REDISCLI_AUTH`, `KDESKDASH_DRM_DEV`, `KDESKDASH_TOUCH_DEV`)
- Modify: `CMakeLists.txt` (`deploy` target: scp binary, install/refresh the unit + env on
  first deploy, `systemctl daemon-reload`, restart)
- Modify: `scripts/sync-sysroot.sh` (verify `libhiredis-dev` headers/libs are present)
- Modify: `README.md` (Redis install, service install/enable, env file, deploy flow)

**Approach:**
- `kdeskdash.service`: `User=root`, `ExecStart=/home/ken/kdeskdash`,
  `EnvironmentFile=/etc/kdeskdash/kdeskdash.env`, `After=redis-server.service` +
  `Wants=redis-server.service`, `Restart=always`, `RestartSec` small, target
  `multi-user.target`.
- On the Pi: `apt-get install -y redis-server libhiredis-dev`; enable `redis-server`; install
  the env file to `/etc/kdeskdash/`; `systemctl enable --now kdeskdash`.
- Deploy target follows kpidash: `ssh ... sudo systemctl stop kdeskdash` → `scp` →
  `sudo systemctl start kdeskdash`, plus a first-time install path for the unit/env file.
- Re-sync the sysroot so the cross-build resolves hiredis.

**Execution note:** Privileged/system-config steps run against `rpidash2`; confirm before any
destructive action.

**Patterns to follow:** `kpidash/CMakeLists.txt` deploy target; this repo's existing `deploy`
target and `scripts/sync-sysroot.sh`.

**Test scenarios:**
- Test expectation: none (deployment/config) — verified by observation.
- Happy path (hardware): power-cycle `rpidash2`; the dashboard comes up automatically on the
  last active mode with no manual steps.
- Edge case (hardware): `systemctl kill kdeskdash` → the service restarts automatically.
- Edge case (hardware): `kdeskdash` ordered after `redis-server` so the first poll connects.

**Verification:** Cold boot brings up the dashboard unattended; crash auto-restarts; Redis is
running and reachable on the Pi; only the dashboard and its supporting services run there.

## System-Wide Impact

- **Interaction graph:** the new shell sits between `main.c`'s loop and the mode screens; the
  Redis poll and `shell_tick` are added to every loop iteration. Gesture handlers attach to
  each mode screen; CLICKED handlers on buttons/tiles coexist via gesture/click exclusivity.
- **Error propagation:** Redis failures are contained in the client module and never abort the
  loop (optional dependency); DRM/evdev failures retain the pre-MVP behavior (touch optional,
  display fatal).
- **State lifecycle risks:** Game of Life buffers must be freed on `deactivate` to avoid leaks
  across mode switches; the `kdeskdash:gol:settings` key must be `DEL`'d after a single use to
  avoid stale re-application; ensure `active_mode` writes don't feed back as spurious remote
  switches (compare-before-switch in the poll).
- **API surface parity:** all modes share the `mode_t` lifecycle; new modes must implement the
  same hooks and add `LV_OBJ_FLAG_GESTURE_BUBBLE` to interactive children.
- **Integration coverage:** stopwatch-across-mode-switch and Redis-driven mode switch are
  cross-layer behaviors verified on hardware (not by unit tests).
- **Unchanged invariants:** the pre-MVP DRM/evdev bring-up, config env defaults
  (`KDESKDASH_DRM_DEV`/`TOUCH_DEV`), and SIGINT/SIGTERM teardown are preserved; `demo_screen`
  is removed once modes exist.

## Risks & Dependencies

- **Game of Life performance** (primary risk): per-generation full-canvas redraw on 1920x440.
  Mitigation: direct `uint32_t` buffer writes, one invalidate per generation, tune minimum
  `cell_size`/`speed_ms` on hardware. Fallback: enforce a minimum cell size.
- **Gesture direction semantics**: LVGL's reported `LV_DIR_*` may not match intuitive
  finger-direction; verify and map on hardware.
- **Cross-build hiredis**: requires `libhiredis-dev` on the Pi + sysroot re-sync; without it
  the link fails. Mitigation: `sync-sysroot.sh` verification step (Unit 6).
- **Running as root**: acceptable for a personal appliance; future hardening to a
  `video`+`input` group user is out of scope.
- **External prerequisites**: `redis-server` and `libhiredis-dev` installed on `rpidash2`
  (neither present today — verified).

## Phased Delivery

- Units 1 → 4 deliver a fully touch-navigable shell with both modes and the Menu (usable with
  no Redis).
- Unit 5 adds remote control, settings injection, and persistence.
- Unit 6 makes it an unattended boot appliance.

Each unit is independently verifiable on hardware; Units 1–4 provide immediate use even before
Redis and the service land.

## Next Steps
→ `/ce-work` to implement, starting with Unit 1.
