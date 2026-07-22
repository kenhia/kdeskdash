# feat: palette mode — named colors + living style guide

Brainstorm: `docs/brainstorms/2026-07-21-palette-mode-requirements.md`
korg: sprint 515, WIs 511–513 (514 = deferred COLOR_* migration)

One X-macro table canonizing the ~30 deliberate design-language colors under
paint-store names, plus a paged swatch-card mode to see them on the panel.

## Unit 1 — pure core `src/palette.c/h` (WI 511)

- [x] `KD_PALETTE(X)` X-macro table (30 entries: surfaces, text, accents/status,
      calc keys, dev charts; legacy clock strays excluded → WI 514).
- [x] Enum `KD_PAL_*` + accessors: count/name/rgb/usage (bounds-checked),
      `kd_pal_find` (case-insensitive, own tolower loop).
- [x] `tests/test_palette.c`: count==enum, unique names, `[A-Z0-9_]` charset,
      rgb ≤ 24-bit, usage non-empty, find hit / lowercase hit / miss,
      spot-check VOID + CLAUDE_CORAL hexes.

## Unit 2 — LVGL mode `src/modes/palette.c` (WI 512)

- [x] 4×2 card grid (445×207) + right paging rail (wrapping Prev/Next, "n/4").
- [x] Card: name mont_28 in-color, usage mont_14 secondary, sample mont_20
      in-color, filled box (hairline border), 3px outlined box, hex readout.
- [x] Cards beyond count hidden on the last page; chrome colors come from the
      palette itself (dogfood).
- [x] Gesture guards + GESTURE_BUBBLE throughout; no tick.

## Unit 3 — wire-up + deploy (WI 513)

- [x] main.c registration ("palette", "Palette"), menu FUN_IDS += "palette",
      CMakeLists sources + test_palette, README + CLAUDE.md touch-ups.
- [x] Host ctest green; cross-compile; deploy rpidash2; screenshot verify.

## Live-test questions (feed WI 514 + tweaks)

Density 8 vs 6 cards/page; usage-note value; dark-swatch legibility (light
well?); name vetoes.
