# feat: menu refresh — fit 5+ tiles, adopt the claude-mode design language

Origin: user request 2026-07-03 (quick sprint, no separate brainstorm). Two drivers:

1. **Clipping**: five content modes at 360px tiles + 40px gaps = 1960px on a
   1920px panel — the outer tiles are visibly cut off, and the shell allows up
   to 8 content modes (`SHELL_MAX_CONTENT`).
2. **Design**: adopt the claude-mode dark palette across the launcher (user:
   "I really like the look of the Claude mode"), replacing the older gray
   scheme (`0x1a1d24` bg / `0x2b3340` tiles).

## Design

Same token set as `src/modes/claude.c` (validated against this surface):
surface `#05070d`, panel `#0a0f1a`, hairline `#1b2334`, ink `#e9edf6`,
accent coral `#cf6b4a`.

- Title: coral, letterspaced (the claude-mode zone-label treatment).
- Tiles: 280×190, 24px gaps — five tiles use 1496px; six fit in 1800px, so the
  next mode needs no layout change. Panel bg + 1px hairline border, radius 10.
- Labels: Montserrat 28 ink (calmer than the previous 36; matches the claude
  rows' primary text size).
- Pressed state: coral border + slightly lifted panel — the one place the
  accent appears, so a touch reads as a response without the screen shouting.
- Behavior unchanged: tiles built from the shell's content list, gesture-guard
  (`GESTURE_BUBBLE` + gesture-dir check in the click callback) preserved.

## Notes

- Menu is pure LVGL view (no core, no test) — hardware-verified via
  `scripts/kddss` (`SET kdeskdash:active_mode menu`, shoot, eyeball).
- The claude/menu token duplication is deliberate for now (per-mode `#define`
  blocks are the house convention); if a third mode adopts the palette, promote
  it to a shared `src/ui_theme.h`.
