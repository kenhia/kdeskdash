---
date: 2026-07-21
topic: calc-mode
---

# kdeskdash `calc` Mode — Desk Calculator (Ops)

## Problem Frame

A quick calculator for the desk: something usable when a number comes up mid-task and
grabbing the phone is friction. The phone calculator is well tuned — this mode does **not**
try to reinvent it. It wins on two things the phone can't offer: it is *already on the
desk*, and the 1920×440 landscape panel has room to keep **everything visible at once** —
result, registers, and live unit conversions — with zero mode-switching or menu digging.

Like `icons`, this mode is entirely local: no network feed, no Redis. Pure core
(`src/calc.c`, host-tested), thin LVGL mode (`src/modes/calc.c`), one registration call.
It joins the **Ops** menu group.

## What the wide screen buys us

A phone calculator stacks display-over-keypad. Here we go three-zone landscape:

```text
+---------------------------+------------------------+--------------------------------+
| RESULT            1234.56 |  R0   25.4  [STO][RCL] | x²  7  8  9  ÷   ⌫   C         |
|  hex  0x4D2               |  R1    ---  [STO][RCL] | x³  4  5  6  ×   π             |
|  bin  0100 1101 0010      |  R2    ---  [STO][RCL] | xʸ  1  2  3  −   e             |
|---------------------------|  R3    ---  [STO][RCL] |     ±  0  .  +   =             |
|  as in → mm      31357.8  |  R4    ---  [STO][RCL] |                                |
|  as mm → in        48.60  |  R5    ---  [STO][RCL] |                                |
|  as mm → px          9495 |                        |                                |
|  as px → mm        160.5  |                        |                                |
+---------------------------+------------------------+--------------------------------+
      readouts (~640px)         registers (~440px)          keypad (~840px)
```

- **Left — readouts.** Big main result on top. Below it, *live interpretations* of the
  current value: hex/binary (when integral), and the four unit conversions. No convert
  buttons, no direction toggle — the panel always shows the value read *as* inches, *as*
  mm, *as* px, all at once. Type `25.4`, glance left, see `1.000 in` — done.
- **Middle — registers.** R0–R5, one row each (~70px, comfortable touch). `STO` copies
  the current display into the register; `RCL` puts the register value into the entry
  (as if typed). Values shown inline so the panel doubles as a scratchpad you can *see*.
  Six fits one clean column; the user asked for 4–8, and 8 would force two columns or
  cramped rows — start at 6, revisit after live test.
- **Right — keypad.** Standard 3×4 numpad island (muscle memory intact), binary ops in a
  column beside it, unary/constants in a column of their own, C/⌫ top-right, `=` bottom
  right. 4 rows × ~100px buttons — big targets, this is the workhorse zone.

## Operations (v1)

- Binary: `+  −  ×  ÷  xʸ` — immediate-execution infix (one pending op, like every desk
  calculator): `2 + 3 × 4 =` → `20`. No precedence, no expression display. Phone has that.
- Unary: `x²  x³  ±  ⌫` (backspace edits the entry being typed).
- Constants: `π`, `e` — pressing one replaces the entry.
- `C` clears everything (except registers); `=` resolves the pending op.
- Errors (÷0, overflow, non-finite): display shows `Error`; any digit or `C` recovers.
  Registers are never touched by an error.

**Deferred until after live test:** trig (sin/cos/tan + inverses, and the deg/rad question
they drag in), `1/x`, `√x`, CE as distinct from C, register persistence across restarts
(Redis), copy-result-to-clipboard-host tricks. The layout above leaves the keypad's
bottom-left region and spare left-panel space for exactly this growth.

## Conversions — the constants

- **in ↔ mm**: exact, 25.4 mm/in.
- **px ↔ mm**: use the *measured* panel calibration from `clock.c` — 7.69 px/mm, taken
  with a ruler against this actual panel (150px buttons span 19.5mm). Not derived from
  the 11.26" diagonal spec, which disagrees (~6.9 px/mm); trust the ruler. Constant moves
  to the pure core so both modes could share it later.

## Hex/binary (the stretch — it fits, so it's in)

Shown in the left panel under the main result, only when the value is integral:

- **hex**: for integral values representable in int64 (two's complement for negatives).
- **bin**: nibble-grouped; shown up to 32 bits, `…` beyond (64 bits of binary doesn't fit
  legibly even on this panel). Blank (`—`) when the value is non-integral or out of range.

Zero extra buttons, zero extra state — it's a formatting function on the pure core,
trivially host-tested.

## Engine shape (pure core)

`calc_t` state machine, no floats-as-strings cleverness beyond an entry buffer:

- `entry[]` text buffer while typing (so `1.05` entry behaves right), `acc`, pending op,
  `regs[6]`, error flag.
- One event API: `calc_key(calc_t*, calc_key_t)` for every button, plus
  `calc_store/recall(int)`. Readout API returns formatted strings: main display
  (`%.12g`-style, fits ~15 chars), hex, bin, and the four conversions.
- Deterministic, no LVGL, no time, no RNG — `tests/test_calc.c` drives key sequences and
  asserts display strings, the same shape as `test_stopwatch.c`.

The LVGL mode is glue: build the three zones, forward button events to `calc_key`,
repaint labels from the readout API. Every CLICKED handler gets the swipe-vs-tap gesture
guard + `GESTURE_BUBBLE` (see `lvgl-swipe-vs-tap-gesture-guard.md`) — a keypad is the
worst-case surface for that bug.

## Open questions for the live test

1. Are 6 registers the right count, and is STO/RCL-per-row better than tap-to-recall +
   long-press-to-store (fewer buttons, more hidden)?
2. Button size/spacing — 100px squares assumed; fat-finger test will tell.
3. Does anyone miss operator precedence in practice, or is immediate-execution fine for
   "quick number while working"? (Bet: fine.)
4. Which deferred op is actually missed first — √x is the likely winner.
