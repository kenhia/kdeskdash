# Game of Life Evolution — Requirements

- **Date:** 2026-06-09
- **Status:** Brainstorm complete — ready for `/ce-plan`
- **Mode touched:** `game_of_life` ([src/modes/game_of_life.c](../../src/modes/game_of_life.c), [src/gol.c](../../src/gol.c) / [src/gol.h](../../src/gol.h))
- **Scope class:** Standard (two independent units bundled under "evolve GoL")

## Summary

Evolve the Game of Life mode with (A) an in-mode tap menu for inspecting and
restarting the current run, (B) a new `rgb` parameter that runs three
independent boards combined into an additive RGB display, and (C) automatic
cycle detection that regenerates the board after it settles into a short loop.
Units A and B share no implementation surface and may be planned/shipped
separately; Unit C is a small rider on the single-board (rgb-off) path and the
menu's open/close lifecycle.

## Background (current behavior)

- Single `gol_t` board, toroidal Conway, optional fade trail. Settings:
  `cell_size, padding, density, trail, trail_turns, speed_ms`.
- Renderer owns a full-screen XRGB8888 canvas; live cells draw full green
  (`0xFF00FF00`), trails fade the green channel; one invalidate per generation.
- `activate()` randomizes settings (with an optional one-shot Redis HGETALL
  overlay via `apply_field`) and reseeds; the canvas bubbles swipe gestures to
  the shell for navigation. There is **no tap handling** in the mode today.
- Panel is 1920×440 (very wide, short). Single UI thread; per-frame work is
  bounded by `random_settings()` and the `apply_field` clamps.

---

## Unit A — Tap Menu (controls + info)

### A. Trigger & region

- **R-A1:** A single tap whose **press point** (captured on `LV_EVENT_PRESSED`,
  not the release point) lands within the **right ~1/4** of the screen
  (`x >= disp_w * 3 / 4`, full height) opens the menu. Taps elsewhere are
  ignored and the simulation continues. The canvas must gain
  `LV_OBJ_FLAG_CLICKABLE` (it has none today) for the press event to fire.
- **R-A2:** No visible affordance/hint is shown for the tap region — this is an
  intentional "if you know, you know" feature.
- **R-A3:** Tap detection must coexist with the existing swipe-to-navigate
  behavior. A gesture (swipe) must **not** be treated as a menu-opening tap
  (reuse the `lv_indev_get_gesture_dir(...) != LV_DIR_NONE` guard pattern used
  in [src/modes/menu.c](../../src/modes/menu.c) and [src/modes/dev.c](../../src/modes/dev.c)).
  Known edge case: a sub-threshold drag in the right quarter that never trips
  LVGL's gesture detector reads as a tap — acceptable (real swipes are caught).

### B. Menu layout & info panel

- **R-A4:** While the menu is open the simulation **keeps running**; cells are
  simply not displayed in the menu region (the opaque menu panel covers the
  right ~1/4). The simulation never pauses.
- **R-A5:** The menu is an **LVGL widget overlay** (a panel with labels +
  Reset/Restart/Cancel buttons), created on open and deleted on close — it is
  **not** painted into the GoL canvas. This keeps `render()` canvas-only,
  avoids per-generation flicker over the menu region, and gets `LV_EVENT_CLICKED`
  button handling for free (same pattern as [src/modes/menu.c](../../src/modes/menu.c)).
  Buttons fire on click; the underlying sim keeps running hidden beneath.
- **R-A6:** The info panel displays:
  - the active settings: `cell_size, padding, density, trail, trail_turns, speed_ms`
  - `rgb` on/off
  - a **generation counter** (generations elapsed since the last (re)seed)
- **R-A7:** Text uses the available fonts only (Montserrat 14/20/28/36/48); given
  the 440px height, expect 14/20 for the info block.

### C. Actions

- **R-A8:** **Reset** — re-seed a **new random population** using the **same
  current settings**. (Not a deterministic replay; settings are unchanged.)
- **R-A9:** **Restart** — full regeneration: roll **new random settings** *and* a
  new population (equivalent to a fresh `activate()` of the mode).
- **R-A10:** **Cancel** — dismiss the menu; the simulation (which never stopped)
  becomes fully visible again.
- **R-A11:** Pressing **Reset** or **Restart** starts the new run **and
  auto-closes the menu** (same dismissal as Cancel). Re-open with another tap.
- **R-A12:** The generation counter resets to 0 on Reset, Restart, and on normal
  mode (re)activation. While the menu is open it **live-updates** in `tick()`
  (the sim keeps stepping), so it reflects the running generation count.

### D. Navigation interaction

- **R-A13:** Swipe navigation remains available while the menu is open; leaving
  the mode via swipe dismisses the menu (state resets on next activation). The
  panel and each button must set `LV_OBJ_FLAG_GESTURE_BUBBLE` so swipes reach
  the shell (same as menu tiles).

---

## Unit B — `rgb` tri-board parameter

### E. Parameter

- **R-B1:** Add `rgb` (bool) to the GoL settings. Default is randomized with a
  **20% chance** of being on per run. The new `gol_rand_u32` draw must be the
  **last** randomization in `random_settings()` so it does not shift the
  existing settings' random stream (keeps prior runs' distributions intact).
- **R-B2:** `rgb` is injectable via Redis like other settings (extend
  `apply_field` in [src/redis.c](../../src/redis.c) mirroring the existing
  `trail` bool: `cfg->rgb = atoi(val) != 0;` — no numeric clamp), and is
  surfaced in the menu info panel (R-A6).

### F. Simulation

- **R-B3:** When `rgb` is on, run **three independent boards** with the **same
  settings**, each seeded with its **own independent random population**.
- **R-B4:** The three boards **do not interact** — each steps Conway/trails
  independently. They are combined only for display.
- **R-B5:** When `rgb` is off, behavior is exactly as today (single green board).

### G. Display

- **R-B6:** Channel assignment: board 0 → **red**, board 1 → **green**,
  board 2 → **blue**.
- **R-B7:** **Per-channel composition** (not cross-board summation): each board
  owns a disjoint channel and sets only that channel's intensity — live cell =
  full intensity, trail = faded intensity (reusing the existing
  `255 * t / trail_turns` math, which is already ≤ 255, so **no clamping/
  saturation is needed**). Final pixel = `(R_intensity, G_intensity,
  B_intensity)`; background stays black where all three boards are dark. (All
  three alive → white; R+G → yellow; etc.)

### H. Timing

- **R-B8:** `speed_ms` keeps the **same wall-clock cadence** whether `rgb` is on
  or off — absorb the extra ~3× per-generation compute rather than auto-scaling.

---

## Unit C — Cycle detection + auto-Restart

Detects when the simulation has settled into a short-period loop (still lifes,
blinkers, and other oscillators of period ≤ 16) and, after a grace period,
automatically regenerates a fresh board.

### I. Enablement

- **R-C1:** Cycle detection is governed by an **internal-only** flag
  `cycle_detect`, **not** a user/Redis setting. It is set to the **complement of
  `rgb`**: `cycle_detect = !rgb`. So detection runs only on the single-board
  (rgb-off) path; with `rgb` on it is fully disabled (no hashing, no timer).
- **R-C2:** `cycle_detect` is (re)evaluated whenever settings are (re)rolled —
  i.e. on activate, Restart, and Redis overlay — since those can flip `rgb`.

### J. Detection algorithm

- **R-C3:** Span is **16 generations**. Maintain a fixed, zero-initialized
  circular buffer of 16 hash slots plus an incrementing counter; the write slot
  is `counter % 16`.
- **R-C4:** After each generation is computed, hash the board's **`cur` alive
  array** (cols×rows bytes; trails are excluded — they don't define the cycle)
  using **64-bit FNV-1a** (no crypto dependency; a hash collision only causes a
  benign early Restart).
- **R-C5:** Compare the new hash against the up-to-16 stored hashes. A match
  signals a detected cycle of period ≤ 16. Then store the new hash at
  `counter % 16` and increment the counter (store regardless of match).
- **R-C6:** The hash buffer and counter are **cleared/zeroed on every (re)seed**
  (activate, Reset, Restart) so a prior run's hashes cannot cause a false match.
  Zeroed slots never collide with a real board hash in practice.

### K. Timed auto-Restart

- **R-C7:** On first detection, arm a **~30-second** TTL. Subsequent detections
  while already armed do not re-arm or extend it.
- **R-C8:** When the TTL expires, perform a **Restart** (full regeneration — new
  random settings + population, per R-A9), which also clears the armed state and
  the hash buffer (R-C6).
- **R-C9:** While the **menu is open**, the TTL is **paused and reset**: opening
  the menu suspends the countdown, and closing it starts a **fresh full ~30s**
  (only relevant if a cycle was/stays detected). This prevents an auto-Restart
  from yanking the board out from under someone inspecting the menu.
- **R-C10:** A manual **Reset** or **Restart** (or leaving the mode) clears any
  armed TTL and detection state.

---

## Out of scope

- Double-tap / multi-tap gestures (explicitly declined; single tap suffices).
- Editing settings from the menu (no toggles/sliders; only Reset/Restart/Cancel).
- Deterministic replay of an exact prior population (Reset re-seeds randomly).
- Per-region menus other than the right strip; alternate menu placements.
- Cycle detection while `rgb` is on (intentionally disabled, R-C1).
- Detecting cycles of period > 16, or translating patterns (e.g. gliders) that
  never return to an identical board within the 16-generation span.
- Exposing `cycle_detect` as a user/Redis setting (internal-only by design).

## Technical considerations (for `/ce-plan`)

- **State:** `uint32_t generation` counter, a `bool menu_open` flag, and the
  overlay's `lv_obj_t *` handle added to `gol_mode_state_t`. Reset/Restart map
  to the existing `reseed()` / re-randomize-then-`reseed()` paths. Cycle
  detection adds `bool cycle_detect`, `uint64_t cycle_hashes[16]`, a
  `uint32_t cycle_counter`, plus a TTL: `bool cycle_armed` and a
  `uint32_t cycle_deadline` tick.
- **Multi-board lifecycle:** replace the single `gol_t gol` + `bool has_grid`
  with `gol_t boards[3]` + `int board_count` (1 when rgb off, 3 when on).
  Because Restart can flip `rgb` on↔off, `reseed()` must free **all** currently
  allocated boards, then alloc/seed exactly `board_count` boards (each with an
  independent rng draw). Free all boards on deactivate/teardown.
- **RGB memory/compute:** three boards = 3× buffers (fine on the 8GB Pi5) and
  3× step cost; `render()` composes R/G/B channels per cell. The single-board
  path (rgb off) should remain the fast common case.
- **Renderer refactor:** `render()` currently hard-codes the green channel;
  generalize so a board writes to an arbitrary channel, then compose the three
  channels when rgb is on. **Acceptance:** with rgb off, output must be
  **pixel-identical** to today (same `255*t/trail_turns` rounding, `0xFF00FF00`
  live cells) — guard against regression from the refactor.
- **Tap hit-testing:** translate the press point to the right-quarter region;
  ensure the menu panel captures button clicks without blocking the tick.
- **Redis:** add `rgb` to `apply_field` mirroring the `trail` bool
  (`atoi(val) != 0`); keep the one-shot overlay + DEL semantics. Update
  README's GoL settings table. (`cycle_detect` is internal-only — derived from
  `rgb` after the overlay is applied, never read from Redis.)
- **Cycle detection:** compute `cycle_detect = !rgb` whenever settings are
  (re)rolled. On the rgb-off path only, after each `gol_step` hash `cur` with
  64-bit FNV-1a, compare against the 16-slot circular buffer, then store. Clear
  the buffer + counter on every reseed (R-C6). The ~30s TTL is checked in
  `tick()` using `lv_tick` deltas; pause/reset it while `menu_open` (R-C9); on
  expiry call the Restart path. Skip all of this entirely when `rgb` is on.
- **Separability:** Unit A and Unit B can be implemented and hardware-verified
  independently; recommend two work units (or two sprints). Unit C depends on
  the single-board path and the menu's open/close flag, so it lands **after**
  (or alongside) A and B — not before.

## Open follow-ups (non-blocking)

- README/`kdeskdash.env.example` docs for the new `rgb` field and the tap menu.
- Decide final randomization weight if 20% feels too rare/common after hardware
  testing (tunable constant).
