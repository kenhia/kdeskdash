---
title: "feat: kdeskdash pre-MVP — DRM display + evdev touch slice"
type: feat
status: active
date: 2026-06-06
origin: docs/brainstorms/2026-06-06-multimode-dashboard-premvp-requirements.md
---

# feat: kdeskdash pre-MVP — DRM display + evdev touch slice

## Overview

Stand up the first runnable slice of `kdeskdash`: a single LVGL binary that boots
fullscreen on the `rpidash2` Raspberry Pi 5 (1920x440 capacitive touch panel), draws basic
shapes, text, and an image, and responds visibly to touch. It reuses the proven `kpidash`
stack (LVGL v9.2.2 `lv_linux_drm` on DRM `card1`, aarch64 cross-compile + Pi-sysroot
workflow) and adds the one piece `kpidash` lacks: the `lv_evdev` touch input driver.

The slice is intentionally structured as a thin app-shell skeleton (init/teardown + one
screen + clean file layout) so the later multi-mode framework grows on a known-good
foundation — but it deliberately does **not** build the mode registry, lifecycle, or
gesture router yet.

## Problem Frame

`kdeskdash` is a multi-mode interactive desk dashboard (see origin:
docs/brainstorms/2026-06-06-multimode-dashboard-premvp-requirements.md). The two hardest
integration points are display bring-up on a non-standard wide-short panel and touch
input — neither proven in `kpidash` (which is display-only on a standard monitor). This
plan de-risks both before any mode/feature work begins.

## Requirements Trace

- R1. Boot fullscreen on `rpidash2` via LVGL v9.2.2 `lv_linux_drm` on DRM `card1` at native 1920x440.
- R2. Render basic shapes (rectangle, circle/arc, line).
- R3. Render text using a font.
- R4. Render an image asset.
- R5. Initialize touch via `lv_evdev` bound to ILITEK at `/dev/input/event1`.
- R6. Detect touch and respond with clearly visible on-screen feedback.
- R7. Ship as a single binary cross-compiled on a dev host, deployed to `rpidash2`, reusing `kpidash`'s toolchain + Pi-sysroot workflow.
- R8. Install a SIGINT/SIGTERM handler that deinitializes LVGL, releases DRM master, and restores the console on exit (exit triggered via Ctrl-C from the attached keyboard).

## Scope Boundaries

- No Redis / shared store, no mode framework, no gesture router, no client apps, no networking, no persistence (all deferred — see origin).
- No mode registry or per-mode activate/deactivate lifecycle in this slice.
- The 3D-printed case is a separate project, out of scope.

## Context & Research

### Relevant Code and Patterns

- `kpidash` (reference project, repo-relative within `../kpidash`): primary pattern source.
  - Entry/init/teardown + main loop: `src/main.c` — `lv_init()` → `lv_linux_drm_create()` →
    `lv_linux_drm_set_file(disp, drm_dev, -1)` → build UI → `while(g_running){ lv_timer_handler(); usleep() }`
    → `lv_deinit()`. SIGINT/SIGTERM via `sigaction` setting a `volatile sig_atomic_t` flag.
  - Cross-compile toolchain: `cmake/aarch64-toolchain.cmake` — `aarch64-linux-gnu-gcc`,
    `PI5_SYSROOT` default `$HOME/pi5-sysroot`, pkg-config pointed at the sysroot.
  - Build wiring: `CMakeLists.txt` — `add_subdirectory(lib/lvgl)`, `pkg_check_modules(DRM REQUIRED libdrm)`,
    link `lvgl` + `${DRM_LIBRARIES}` + `pthread`, `-Wl,--allow-shlib-undefined` for the Trixie-sysroot/Ubuntu-host glibc gap, and a `deploy` custom target (ssh stop → scp → ssh start).
  - LVGL config: `lv_conf.h` — `LV_USE_LINUX_DRM 1`.
- LVGL touch driver API (from `kpidash/lib/lvgl/src/drivers/evdev/lv_evdev.h`, v9.2.2):
  - `lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event1")`
  - `lv_evdev_set_swap_axes(indev, bool)` and `lv_evdev_set_calibration(indev, min_x, min_y, max_x, max_y)` for wide-panel mapping.

### External References

- LVGL v9.2.2 in-tree drivers `lv_linux_drm` and `lv_evdev` (pinned via the reused submodule). `lv_evdev` uses raw `linux/input.h` ioctls — no `libinput`/`libevdev` dependency.

## Key Technical Decisions

- Reuse `kpidash`'s stack and toolchain verbatim, trimming unused deps (no hiredis, cJSON, or libpng for this slice). Rationale: proven on the same OS (Debian 13 Trixie) and Pi 5 class; minimizes new risk.
- Pin LVGL via submodule at v9.2.2 (matches `kpidash`; ships both needed drivers).
- Pre-MVP image is a **converted LVGL C-array** asset, not a runtime PNG decode. Rationale: avoids adding `libpng` to the binary and the sysroot for a one-image demo (resolves origin deferred question on R4). Source image: `assets/brain-rot-cropped.png` (285x288 RGBA), converted with LVGL's `lib/lvgl/scripts/LVGLImage.py` to a C array.
- Text uses a **built-in LVGL Montserrat font** enabled in `lv_conf.h`, not a bundled converted font. Rationale: no font-conversion pipeline needed for the slice (R3).
- Run under `sudo` for DRM master, mirroring `kpidash`. Rationale: simplest known-good path; non-root via `video`/`render` group + udev is a later refinement.
- Touch device path `/dev/input/event1` and DRM device `/dev/dri/card1` are env-overridable with those defaults. Rationale: avoids hard-coding while keeping zero-config startup on `rpidash2`.

## Open Questions

### Resolved During Planning

- Image format (origin R4 deferred): converted C-array image — decided above.
- Privilege model (origin R1 deferred): run under `sudo` for this slice — decided above.
- Input dependency: `lv_evdev` needs only `linux/input.h` (from `linux-libc-dev`, already on the Pi) — no `libevdev`/`libinput`.

### Deferred to Implementation

- Touch coordinate mapping on the 1920x440 panel (origin R5/R6): **Resolved on hardware** — taps register at correct locations with no axis swap or calibration needed.
- Whether `lv_deinit()` alone fully restores the console/VT, or an explicit DRM fd close / VT restore is required on this kernel — **Resolved on hardware** — SIGTERM teardown via `lv_deinit()` exits cleanly; subsequent device access works.
- Native 1920x440 mode acceptance by the vc4 KMS driver (origin R1 risk) — **Resolved** — `card1-HDMI-A-*/modes` reports a native `1920x440` mode and the app renders fullscreen without letterboxing.

## Implementation Units

- [x] **Unit 1: Project scaffold + LVGL submodule + lv_conf**

**Goal:** Create the `kdeskdash` build skeleton with LVGL v9.2.2 vendored and configured for DRM + evdev.

**Requirements:** R1, R3, R5, R7

**Dependencies:** None

**Files:**
- Create: `CMakeLists.txt` (adapted from `kpidash` — `lvgl` + `libdrm` + `pthread`; no hiredis/cJSON/png)
- Create: `lv_conf.h` (set `LV_USE_LINUX_DRM 1`, `LV_USE_EVDEV 1`, enable one `LV_FONT_MONTSERRAT_*`)
- Create: `cmake/aarch64-toolchain.cmake` (copy from `kpidash`)
- Create: `.gitmodules`, add submodule `lib/lvgl` pinned to tag `v9.2.2`
- Create: `.gitignore` (build dirs, sysroot)

**Approach:**
- Mirror `kpidash`'s CMake structure; drop Redis/JSON/PNG targets and font `.c` files. Keep `-Wl,--allow-shlib-undefined` and `add_compile_definitions(LV_CONF_PATH=...)`.
- Enable the chosen built-in Montserrat font so no font assets are needed.

**Patterns to follow:** `kpidash/CMakeLists.txt`, `kpidash/lv_conf.h`, `kpidash/.gitmodules`.

**Test scenarios:**
- Test expectation: none — scaffolding/config only; validated by Unit 2/3 build success.

**Verification:** `lib/lvgl` checks out at `v9.2.2`; `lv_conf.h` enables DRM, EVDEV, and a Montserrat font; the host CMake configure step (TESTS-less) resolves LVGL without error.

---

- [x] **Unit 2: Cross-compile + sysroot + deploy tooling**

**Goal:** Produce a deployable aarch64 binary from a dev host and a one-command deploy to `rpidash2`.

**Requirements:** R7

**Dependencies:** Unit 1

**Files:**
- Create: `scripts/sync-sysroot.sh` (rsync `/lib`, `/usr/lib`, `/usr/include` from `rpidash2`)
- Modify: `CMakeLists.txt` (add a `deploy` custom target: scp binary to `rpidash2`)
- Create: `README.md` (build + deploy + run instructions, adapted from `kpidash`)

**Approach:**
- Prerequisite on `rpidash2`: `sudo apt-get install -y libdrm-dev` so the synced sysroot contains DRM headers/`.pc` (the dev package is currently absent — verified). `linux/input.h` is already present.
- Sysroot defaults to `$HOME/pi5-sysroot`; toolchain points pkg-config at it.
- `deploy` target rsyncs/scps the binary to `ken@rpidash2`.

**Execution note:** Environment-prep heavy — confirm sysroot contains `libdrm.pc` and `xf86drm.h` after sync before proceeding to Unit 3.

**Patterns to follow:** `kpidash/README.md` "Cross-compile" section, `kpidash/CMakeLists.txt` `deploy` target.

**Test scenarios:**
- Test expectation: none — build/deploy tooling; proven by a clean cross-build in Unit 3.

**Verification:** `cmake -B build-pi -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake` configures with DRM found in the sysroot; the deploy target lands a binary on `rpidash2`.

---

- [x] **Unit 3: App skeleton — DRM display bring-up + clean teardown**

**Goal:** A minimal LVGL app that opens the DRM display fullscreen at 1920x440 and exits cleanly on signal.

**Requirements:** R1, R8

**Dependencies:** Unit 2

**Files:**
- Create: `src/main.c` (entry point, init/loop/teardown)
- Create: `src/config.h` / `src/config.c` (env overrides: `KDESKDASH_DRM_DEV` default `/dev/dri/card1`, `KDESKDASH_TOUCH_DEV` default `/dev/input/event1`)

**Approach:**
- Follow `kpidash/src/main.c`: `lv_init()` → `lv_linux_drm_create()` → `lv_linux_drm_set_file(disp, drm_dev, -1)` → main loop `lv_timer_handler()` + `usleep`.
- SIGINT/SIGTERM via `sigaction` → `g_running = 0`; on exit `lv_deinit()` and ensure the DRM fd is closed / console restored (verify whether `lv_deinit` suffices — see deferred question).
- Before relying on DRM, verify panel mode (see Risks / `modetest`).

**Technical design:** *(directional guidance, not implementation specification)*
- init: `lv_init` → drm create + set_file(card1) → [Unit 4 builds screen] → [Unit 5 creates indev] → loop → signal → teardown.

**Patterns to follow:** `kpidash/src/main.c`, `kpidash/src/config.c`.

**Test scenarios:**
- Happy path: binary launched under `sudo -E` on `rpidash2` clears the screen fullscreen at 1920x440 (no letterboxing).
- Error path: when `KDESKDASH_DRM_DEV` points at an invalid card, the app logs a clear error and exits non-zero rather than hanging.
- Edge case (R8): Ctrl-C / SIGTERM exits within one loop iteration and the console/VT is usable afterward (no blank panel, DRM master released).

**Verification:** On `rpidash2`, running the binary shows a full-screen blank LVGL display; Ctrl-C returns to a working console.

---

- [x] **Unit 4: Demo screen — shapes, text, image**

**Goal:** Draw the required visual elements on the active screen.

**Requirements:** R2, R3, R4

**Dependencies:** Unit 3

**Files:**
- Create: `src/demo_screen.c` / `src/demo_screen.h` (builds the demo content; called after display init)
- Add: `assets/brain-rot-cropped.png` (source, already staged) and generated `assets/img_brain_rot.c` (LVGL C-array descriptor from `LVGLImage.py --ofmt C --cf ARGB8888`)
- Modify: `src/main.c` (call demo-screen builder after display init)
- Modify: `CMakeLists.txt` (add new sources)

**Approach:**
- Shapes via LVGL objects/styles or a canvas: a rectangle, a circle/arc, and a line (R2).
- A text label using the enabled Montserrat font (R3).
- An `lv_image` widget bound to the generated `img_brain_rot` C-array asset (from `assets/brain-rot-cropped.png`, R4).
- Lay out for the wide 1920x440 aspect (horizontal arrangement) so elements aren't clipped.

**Patterns to follow:** LVGL v9 object/style usage; `kpidash/src/ui.c` for screen composition conventions.

**Test scenarios:**
- Happy path: on-device, the screen shows the rectangle, circle/arc, line, the text label (legible), and the image, all within the 1920x440 bounds.
- Edge case: image and text remain fully visible (not clipped) given the short 440px height.

**Verification:** Visual confirmation on `rpidash2` that all four element types render correctly within the panel.

---

- [x] **Unit 5: Touch input + visible response**

**Goal:** Wire `lv_evdev` touch and make a touch produce clearly visible feedback.

**Requirements:** R5, R6

**Dependencies:** Unit 4

**Files:**
- Modify: `src/main.c` (create the evdev pointer indev after display init)
- Modify: `src/demo_screen.c` (add a touch target + event callback with visible feedback)

**Approach:**
- `lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_dev)` with `touch_dev` from config (`/dev/input/event1`).
- Add a clickable target (e.g., a button or a clickable shape) whose `LV_EVENT_CLICKED`/`LV_EVENT_PRESSED` callback changes its appearance and/or displays the last tap coordinates.
- If taps land in the wrong place, apply `lv_evdev_set_swap_axes` and/or `lv_evdev_set_calibration` tuned to the panel (see deferred question).

**Patterns to follow:** LVGL v9 `lv_indev` + event callbacks; `lv_evdev.h` API as documented.

**Test scenarios:**
- Happy path: tapping the target visibly changes it (color/state) and/or shows coordinates within ~one frame.
- Edge case: tap coordinates map to the correct on-screen location across the full 1920px width and 440px height (verify after calibration).
- Error path: if `/dev/input/event1` cannot be opened, the app logs a clear warning and still runs the display (touch optional, not fatal).

**Verification:** On `rpidash2`, touching the target produces immediate, correct visible feedback; taps across the panel map to the right locations.

## System-Wide Impact

- **Interaction graph:** Greenfield single binary; no existing surfaces affected. The `main.c` init order (display → screen → indev → loop) is the seam the future mode shell will hook into.
- **State lifecycle risks:** DRM master must be released on exit (R8) or the panel/console can be left unusable — covered in Unit 3.
- **API surface parity:** None yet (no Redis/clients in this slice).
- **Unchanged invariants:** This slice intentionally introduces no mode registry, gesture router, or shared store; later work adds them without reworking the display/touch bring-up proven here.

## Risks & Dependencies

| Risk | Mitigation |
|------|------------|
| Non-standard 1920x440 panel: vc4 KMS may not enumerate/accept a native mode, breaking R1 fullscreen | Verify with `modetest` on `rpidash2` (correct connector, native timing, no scaling) before trusting the DRM path; treat panel mode-setting — not the reused LVGL code — as the primary R1 risk |
| Sysroot lacks DRM headers (`libdrm-dev` absent on `rpidash2`) → cross-build fails | `apt-get install libdrm-dev` on `rpidash2` before `sync-sysroot.sh` (Unit 2 prerequisite) |
| Touch coordinates mis-mapped on the wide-short panel | Use `lv_evdev_set_swap_axes` / `lv_evdev_set_calibration`; tune against observed tap coords (Unit 5) |
| DRM master requires privilege | Run under `sudo -E` (mirrors `kpidash`); non-root via `video`/`render` group deferred |
| Glibc skew (Trixie sysroot vs older dev host) at link time | Keep `-Wl,--allow-shlib-undefined` from `kpidash`; symbols resolve at runtime on the Pi |

## Documentation / Operational Notes

- `README.md` documents the cross-compile, sysroot sync (including the `libdrm-dev` prerequisite on `rpidash2`), deploy, and `sudo -E` run steps.
- Run command on device: `sudo -E ./kdeskdash` (Ctrl-C to exit).

## Sources & References

- **Origin document:** [docs/brainstorms/2026-06-06-multimode-dashboard-premvp-requirements.md](docs/brainstorms/2026-06-06-multimode-dashboard-premvp-requirements.md)
- Reference project: `kpidash` — `src/main.c`, `CMakeLists.txt`, `cmake/aarch64-toolchain.cmake`, `lv_conf.h`, `README.md`
- LVGL v9.2.2 drivers: `lib/lvgl/src/drivers/display/drm/lv_linux_drm.h`, `lib/lvgl/src/drivers/evdev/lv_evdev.h`
