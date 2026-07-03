---
date: 2026-07-03
topic: icons-nerdfont-browser
---

# kdeskdash `icons` Mode — Nerd Font Icon Browser

## Problem Frame

Nerd Fonts bundles ~10,390 glyphs. The dashboard (and its sibling `kpidash`) will want
icons in real UI — OS logos, service/status symbols, dev-tool marks — but the two big
sets (Font Awesome, Material Design) aside, every set is under 500 glyphs, and picking the
*right* icon means seeing it at panel size, on the panel, next to its neighbours. The Nerd
Fonts web cheat-sheet is a great **search** ("I know I want `git-branch`") but a poor
**browse** ("show me the candy aisle so I can find the ones I like"). This mode is the
candy aisle: a touch grid to page through a set, a preview to see a chosen glyph at real
sizes, and a favourites list saved to disk so a good find is never lost.

Unlike `dev`/`claude`, this mode has **no network feed** — it is entirely local: a font
file, a set table, and a favourites file. Its one new capability is a runtime font engine.

### The font-engine decision (why this isn't a bake)

`kpidash` already proved the static path: `fonts/generate.sh` runs `lv_font_conv` to bake
selected Nerd ranges into committed C font files (`SymbolsNerdFont-Regular.ttf` +
Montserrat-Bold, `--no-compress --no-prefilter`, one file per pixel size, guarded by
`LV_FONT_*` compile macros). That is the right tool for a **small, curated, pixel-crisp**
set (their status cards use ~20 baked glyphs). It is the wrong tool for *browsing all
10,390*: every glyph you might want must be baked in advance, at every size, ballooning the
binary (their own measurement: ASCII + ~1,200 icons ≈ 1.1 MB **per size**).

Our pinned LVGL 9.2.2 ships **`LV_USE_TINY_TTF`** (an stb_truetype rasteriser, bundled in
`lib/lvgl/src/libs/tiny_ttf/`). With it, we vendor the one 2.4 MB symbols TTF and call
`lv_tiny_ttf_create_file(path, size)` at startup; every glyph in the file is then usable at
any size, rasterised on demand and cached — **no range list, no per-size bake, no
`generate.sh`**. "Which set the mode shows" stops being a build decision and becomes plain
data (a table of `{name, start, end}`). This is the enabling decision for the whole mode,
and it is arguably *less* project machinery than the bake, in keeping with the
pure-core / thin-mode / "less framework" posture.

The two paths stay complementary: this mode is the exploration tool; when specific icons
graduate into permanent UI, the favourites file it produces is the exact curation list to
feed a static bake (kpidash-style) for crispness. Best of both, in sequence.

## Layout

Native 1920×440, claude-mode design language (dark surface, panel tiles with a hairline
border, coral used sparingly for the accent/selection). Three columns: a browse **grid**
(left, the workhorse), a **preview** stack (centre), and a **control** column (right).

```text
+--------------------------------------------------+-------------------+----------------+
| Devicons                          set 5/14 · 198 |  PREVIEW          | [‹ Set][Set ›] |
|                                                  |                   |                |
|   A  B  C  D  E  F  G  H  I  J  K  L  M  N       |     (72)          |  [ ★ only  ]   |
|   O  P  Q  R  S  T  U  V  W  X  Y  Z  a  b       |     (48)          |  [ ★ fav    ]  |
|   c  d  e  f  g  h  i  j  k  l  m  n  o  p        |   (36) (28) (20)  |                |
|   q  r  s  t  u  v  w  x  y  z  0  1  2  3        |   U+E706          |  [ Save ]      |
|                                                  |   page 1/4        | [<Prev][Next>] |
+--------------------------------------------------+-------------------+----------------+
```

Prose is authoritative. **Grid**: 4 rows of touchable cells (~90 px, ~14 columns → ~56
icons/page), each an icon-font label; the selected cell lifts to the coral border/press
treatment (same tap-answer as menu tiles). **Header**: current set name, position
("set 5/14"), and the set's glyph count. **Preview**: the selected glyph rendered at
several sizes (e.g. 72 / 48 / 36 / 28 / 20) stacked so its legibility at real UI sizes is
obvious, plus its codepoint (`U+E706`) in tabular figures and the in-set page indicator.
**Controls** (fixed column — the panel has the width; only collapse to a fly-out if it
feels cramped): *Pick Set*, *★ only* (favourites-only filter toggle), *★ fav* (mark/unmark
the selected glyph), *Save*, and *Prev/Next* page. A hairline separates the columns.

## Requirements

**Font engine**
- R1. Enable `LV_USE_TINY_TTF` (and `LV_TINY_TTF_FILE_SUPPORT`) in `lv_conf.h`. The
  tiny_ttf source is already globbed into the LVGL build; no CMake change is needed for the
  engine itself.
- R2. Vendor the symbols TTF in-repo (`fonts/ttf/SymbolsNerdFont-Regular.ttf`, copied from
  kpidash — the symbols-only Nerd Font: icons, no letters). It is deployed to the Pi as a
  **file** (default `/usr/local/share/kdeskdash/SymbolsNerdFont-Regular.ttf`), not embedded
  as a C array — embedding 2.4 MB bloats the binary and slows every compile, and TinyTTF
  reads a file directly.
- R3. The mode creates a small set of font objects at fixed preview sizes once on first
  activate (`lv_tiny_ttf_create_file` per size, or one font + `lv_tiny_ttf_set_size`), plus
  the grid-cell size. Fonts persist for the mode's lifetime (created lazily, never per
  tick). If the TTF is missing/unreadable, the mode shows a designed unavailable state
  (like `dev`/`claude`), and other modes are unaffected.

**Set model (pure)**
- R4. A static set table enumerates the Nerd Font sets as `{ name, start_cp, end_cp }`
  (Powerline, Font Logos, Seti-UI, Devicons, Weather, Codicons, Octicons, Font Awesome,
  Font Awesome Extension, Material Design, Pomicons, …). Ranges come from the Nerd Fonts v3
  layout (the map in `../kpidash/specs/002-exploration-sprint/research-fonts.md` is the
  starting reference).
- R5. The two large sets (Font Awesome, Material Design) are split into fixed-size
  sub-pages presented as separate entries (`Font Awesome 1`, `Font Awesome 2`, …) so no
  single set is unwieldy — a purely cosmetic split over a contiguous range, since 4 rows is
  the natural page size regardless.
- R6. Ranges are declared generously; **sparse gaps are handled at render time**, not in
  the table. The grid asks the font whether each codepoint has a glyph
  (`lv_font_get_glyph_dsc` / has-glyph) and shows only present glyphs, so a set with holes
  (Octicons, Material Design) pages cleanly with no empty cells. The set's displayed count
  is the number of *present* glyphs.

**Grid + selection (pure paging math, thin LVGL render)**
- R7. The grid pages within the current set: page math (glyphs-per-page from rows×cols,
  page count, current page, clamp) lives in the pure core and is host-tested. The LVGL
  layer only paints the current page's glyphs into cells.
- R8. Tapping a cell selects that glyph (updates the preview). The tap handler uses the
  documented swipe-vs-tap guard (`lv_indev_get_gesture_dir` — see
  `docs/solutions/best-practices/lvgl-swipe-vs-tap-gesture-guard.md`) so a swipe that
  releases on a cell never registers as a selection.
- R9. In-mode navigation uses **on-screen buttons**, never swipes: the shell reserves
  swipe left/right (cycle modes) and swipe-down (menu). A two-way **Set ‹ / ›** row steps
  through sets in either direction (wrapping) so a distant set is a few taps, not a full
  lap; a bare **‹ / ›** row pages within the current set. Cells and controls set
  `LV_OBJ_FLAG_GESTURE_BUBBLE` so shell navigation still works from anywhere on the screen.

**Preview**
- R10. The preview shows the selected glyph at several fixed sizes stacked (largest to
  smallest), its codepoint as `U+XXXX` (tabular), the current set name, and the in-set page
  indicator. Empty selection (nothing tapped yet on a page) shows a quiet placeholder.

**Favourites (pure model + file persistence)**
- R11. A favourite is a codepoint. The favourites set lives in the pure core (add / remove /
  toggle / contains / count / ordered iteration). *★ fav* toggles the selected glyph;
  favourited glyphs carry a coral marker in the grid.
- R12. *★ only* filters the browse view to favourites across **all** sets (a virtual
  "Favourites" view), so a curated shortlist can be reviewed together regardless of origin
  set. Toggling it off returns to normal set browsing.
- R13. *Save* writes the favourites to a file on disk (default
  `/var/lib/kdeskdash/icon-favorites.txt`, override via env). Format: one codepoint per
  line as lowercase hex (e.g. `f31b`), optionally with a trailing `# <set>` comment —
  chosen so the file drops straight into `lv_font_conv -r` ranges for a future bake, and so
  it is trivially diff-able and hand-editable. The purpose is primarily **"find the good
  ones again"**; feeding a bake is the happy secondary use.
- R14. On activate, the mode loads the favourites file if present (missing file → empty
  set, not an error). Parse/format is pure and host-tested (round-trips: unknown lines
  ignored, blank/comment lines skipped, dedup, stable order). Save is fire-and-forget with
  an on-screen confirmation of success/failure; a write failure never crashes the mode.

**Mode integration**
- R15. `icons` registers as a normal content mode (one `shell_register_content_mode` line
  in `main.c`); swipe/menu integration and the menu tile come free. It appears in the swipe
  cycle and the menu launcher like every other content mode.
- R16. New config: `KDESKDASH_ICONS_TTF` (TTF path) and `KDESKDASH_ICONS_FAVORITES`
  (favourites file path), with the defaults above, following the `config.c` env-or-default
  pattern. Deploy places the TTF and ensures the favourites directory exists.
- R17. No tick work is required for correctness (the view is event-driven: taps and button
  presses). `tick` may be NULL or a no-op; all state changes flow from input events.

**Visuals**
- R18. claude-mode palette: surface `#05070d`, panel `#0a0f1a`, hairline `#1b2334`, ink
  `#e9edf6`, secondary `#8b95ab`, muted `#525d73`, accent coral `#cf6b4a` (selection +
  favourite marker + pressed state only). Icons render in ink; the selected cell and
  favourite marker are the coral accents. Codepoints/counters use tabular figures.
  README-quality is a goal but secondary to the earlier feed modes.

## Success Criteria

- Opening the mode and paging a set, every present glyph in that set is visible and
  legible at grid size within a few pages; no empty/broken cells for sparse sets.
- Tapping a glyph shows it at real UI sizes instantly, so "will this read at 20 px on the
  panel?" is answerable at a glance.
- Marking favourites across several sets, flipping *★ only*, and *Save* produces a file on
  the Pi whose codepoints match what was marked — and reopening the mode later reloads them.
- A missing TTF or unwritable favourites path degrades gracefully (unavailable state /
  on-screen error), never taking down the dashboard.
- The favourites file can be pasted into `lv_font_conv -r` ranges to bake a curated font
  with no reformatting.

## Scope Boundaries

- **No glyph-name catalogue this sprint.** Showing the codepoint (`U+XXXX`) is enough to
  "find it again"; wiring the Nerd Fonts `glyphnames.json` (name/label per codepoint) is a
  nice follow-up but not required for browsing, and it is a large data file.
- **No search box.** This mode is deliberately browse-first; the web cheat-sheet remains
  the search tool. (A jump-to-set picker is the only navigation aid.)
- **No text/ASCII font.** Symbols-only TTF; all body text stays on built-in Montserrat.
- **No per-icon colour/tint experiments**, no icon composition — just faithful glyph
  rendering at size.
- **No baking here.** The mode never runs `lv_font_conv`; it only *produces the list* a
  future bake would consume.
- **No Redis.** Favourites persist to a local file; nothing about this mode touches any
  Redis endpoint.

## Key Decisions

- **TinyTTF runtime engine, not a static bake** (R1–R3): the only path where all 10,390
  glyphs are browsable cheaply and where a multi-size preview is trivial (create sizes
  once, reuse across every glyph). Bake stays the tool for the small curated set later.
- **Deploy the TTF as a file, not embed as a C array** (R2): keeps compiles fast and the
  binary lean; TinyTTF reads files natively; one line in the deploy script.
- **Render-time glyph-existence probing over an exact codepoint table** (R6): Nerd sets are
  sparse; probing the font is simpler and always correct than curating gap lists, and it
  makes the set table a handful of generous ranges.
- **File-based favourites in bake-ready hex** (R13): matches "save to disk", works with
  Redis absent, and doubles as the curation list for a future static font.
- **On-screen Prev/Next paging, gesture-guarded taps** (R8–R9): respects the shell's
  reserved swipe gestures; reuses the proven menu/GoLZ tap-vs-swipe pattern.
- **Pure core + thin mode** (R4, R7, R11, R14): set table, page math, favourites set, and
  favourites file parse/format are host-tested with no LVGL; the mode is glue.

## Dependencies / Assumptions

- `lib/lvgl` (9.2.2) tiny_ttf + stb_truetype present and compiled (verified); enabling the
  macro in `lv_conf.h` suffices.
- The vendored `SymbolsNerdFont-Regular.ttf` contains the full Nerd Font symbol range at
  the codepoints the set table assumes (Nerd Fonts v3 layout).
- The service runs as root (DRM/evdev), so it can create `/var/lib/kdeskdash/` and write the
  favourites file; deploy ensures the directory exists.
- Single-threaded LVGL posture unchanged: all rasterisation on the UI thread, on demand,
  cached by TinyTTF.

## Outstanding Questions

### Resolve Before Planning
- (none — product decisions are resolved: TinyTTF engine, all sets via a set picker, grid +
  multi-size preview, file-based favourites keyed on "find them again")

### Deferred to Planning
- [Affects R4/R5] Freeze the exact set table (names + ranges + big-set sub-page size) from
  the Nerd Fonts v3 layout; decide the sub-page split count for FA/MD.
- [Affects R3/R10] Pick the concrete preview sizes and the grid-cell size / column count
  that best fit 4 rows on 1920×440 (a quick on-device eyeball during the build).
- [Affects R6] Confirm the TinyTTF has-glyph probe API on 9.2.2 (`lv_font_get_glyph_dsc`
  returns false for absent glyphs) and that probing a whole page each render is cheap
  enough (cache once per page if needed).
- [Affects R13/R16] Confirm the favourites directory/path and that the systemd unit's
  working dir / permissions allow the write; add a deploy step to create it.

## Next Steps

→ `/ce-plan`-style implementation plan (units below), TTF + `lv_conf` wiring and the pure
  core first so the mode is glue over tested logic.
