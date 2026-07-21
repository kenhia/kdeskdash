# feat: calc mode — desk calculator (Ops)

Brainstorm: `docs/brainstorms/2026-07-21-calc-mode-requirements.md`
korg: sprint 510, WIs 505–508 (509 deferred follow-ups)

Three-zone landscape calculator: readouts (result + hex/bin + live in↔mm / px↔mm)
on the left, registers R0–R5 in the middle, keypad on the right. Pure core + thin
mode; immediate-execution infix; no network, no Redis.

## Unit 1 — pure core `src/calc.c` (WI 505, 506)

- [x] `calc.h`/`calc.c`: `calc_t` (entry buffer, acc, pending op, regs[6], error),
      `calc_init`, `calc_key(calc_t*, calc_key_t)` covering digits/dot/backspace,
      `+ − × ÷ xʸ`, `x² x³ ±`, `π e`, `C`, `=`; `calc_store/recall(int)`.
- [x] Readout API: `calc_display()`, `calc_hex()`, `calc_bin()` (nibble-grouped,
      ≤32 bits), `calc_convert()` for in→mm / mm→in / mm→px / px→mm.
      Constants: 25.4 exact; `CALC_PX_PER_MM 7.69f` (clock.c ruler calibration).
- [x] `tests/test_calc.c`: entry editing, chained ops, xʸ, unary, constants,
      registers survive C and errors, ÷0/overflow recovery, display/hex/bin
      formatting boundaries, conversions round-trip.

## Unit 2 — LVGL mode `src/modes/calc.c` (WI 507)

- [x] Three-zone build_screen; keypad grid with numpad island; register rows with
      STO/RCL; readout labels repainted only on state change (dirty flag).
- [x] Gesture guard + `GESTURE_BUBBLE` on every button (best-practice doc).

## Unit 3 — wire-up + deploy (WI 508)

- [x] `main.c` registration (`calc_mode_create("calc", "Calc")`), `menu.c`
      OPS_IDS += "calc", CMakeLists sources + `test_calc` block.
- [x] Host build + ctest green.
- [x] Cross-compile `build-pi`, deploy to rpidash2 → live usability test.

## Live-test questions (feed WI 509)

Register count 6 vs 8; STO/RCL buttons vs tap/long-press; button sizing; first
missed operator (√x favourite); precedence pain in practice.
