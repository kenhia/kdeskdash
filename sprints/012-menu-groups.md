# feat: menu groups — Fun / Ops 3×3 panels (fix tile clipping)

Origin: quick sprint (2026-07-18). The single-row menu clipped once the content
modes grew to 7 (Game of Life, GoLZ, Clock, Dev, Claude, Icons, Remote): the row
of 280px tiles overflows 1920px, shearing the first and last tiles off both edges
(verified on-device).

## Design

Two side-by-side groups, each a **3×3 grid** (9 slots = room to grow), a hairline
divider between them, and "Fun" / "Ops" group headers (drop the redundant centre
"Menu" title).

- **Fun** (left): Game of Life, GoLZ, Icons  — 3 used, 6 free
- **Ops** (right): Claude, Remote, Clock, Dev — 4 used, 5 free

Tiles fill each grid top-left, row by row (no empty placeholder tiles); the 3×3 is
reserved capacity, and a new mode flows into the next slot. Layout keeps the
claude-mode design language (dark panels, hairline border, coral pressed accent).

## Approach

Rewrite `src/modes/menu.c` only — no mode or shell changes:

- Two static id lists (`FUN_IDS`, `OPS_IDS`) map modes to a group + order.
  Tiles are built via `shell_find_mode(id)` (skips a group entry whose mode isn't
  registered).
- **Safety net:** after placing both lists, any registered content mode
  (`shell_content_at`) whose id is in *neither* list is appended to Ops — so a
  future mode can never silently disappear from the menu (it just lands in Ops
  until assigned).
- Tiles reuse the existing tap handler (swipe-vs-tap guard → `shell_set_active`);
  sizing shrinks to fit a 3-row grid (≈288×112 vs the old 280×190).
- Menu stays the swipe-down target + startup default; no navigation change.

## Verify

Host build + ctest (no logic change — menu has no unit test; the pure cores are
unaffected). Cross-compile + deploy; on-device screenshot at `menu` to confirm no
clipping, correct grouping/order, and that every mode is reachable. Then ship.
