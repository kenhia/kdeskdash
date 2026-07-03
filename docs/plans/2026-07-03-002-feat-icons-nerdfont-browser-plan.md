# feat: `icons` mode — Nerd Font icon browser (TinyTTF)

Origin: docs/brainstorms/2026-07-03-icons-nerdfont-browser-requirements.md

Goal: a browse-first Nerd Font gallery mode — page a set in a touch grid, preview the
selected glyph at real sizes, mark favourites, save them to a bake-ready file. Engine is
LVGL's runtime TinyTTF (all ~10,390 glyphs, no static bake). Pure core + thin mode.

## Architecture at a glance

- **`src/iconset.{c,h}`** — pure, no LVGL/Redis, host-tested. The set table, page math,
  selection index, favourites set, and favourites file parse/format. This is where all the
  logic lives.
- **`src/modes/icons.{c,h}`** — thin LVGL glue: builds the grid/preview/controls, owns the
  TinyTTF font objects, renders `iconset` state, routes taps/buttons back into the core.
- **`lv_conf.h`** — enable `LV_USE_TINY_TTF` + `LV_TINY_TTF_FILE_SUPPORT`.
- **`fonts/ttf/SymbolsNerdFont-Regular.ttf`** — vendored (copy from kpidash), deployed to
  the Pi as a file.
- **`src/config.{c,h}`** — two new env-configured paths (TTF, favourites file).

## Open items to resolve during the build (from brainstorm Deferred-to-Planning)

- Freeze the set table ranges + FA/MD sub-page split (Unit 2).
- Confirm the has-glyph probe API (`lv_font_get_glyph_dsc(font, &dsc, cp, 0)` → false when
  absent) and that per-page probing is cheap; cache the present-codepoint list per page if
  needed (Unit 4).
- Pick preview sizes + grid cell size / column count on-device (Unit 4, eyeball).
- Confirm favourites dir/permissions + add the deploy step (Unit 5).

## Units

1. **TinyTTF + TTF asset wiring** — `lv_conf.h`: `LV_USE_TINY_TTF 1`,
   `LV_TINY_TTF_FILE_SUPPORT 1`. Copy `SymbolsNerdFont-Regular.ttf` into `fonts/ttf/`
   (LICENSE note: SIL OFL, symbols-only). No CMake change for the engine (tiny_ttf is
   globbed into the LVGL lib already). Sanity: a throwaway `lv_tiny_ttf_create_file` call
   renders one glyph on the host build (or defer visual proof to Unit 6 on-device).

2. **`iconset` core — set table + page math (+ tests)** — Declare the Nerd Font sets as
   `{ const char *name; uint32_t start, end; }`, big sets (Font Awesome, Material Design)
   pre-split into `Font Awesome 1..N` / `Material Design 1..N` entries over sub-ranges.
   Ranges from the Nerd Fonts v3 layout (kpidash `research-fonts.md` map as the seed).
   Pure API: set count, set at index, set name/range; page math
   (`iconset_pages(present_count, per_page)`, clamp, wrap set index). `tests/test_iconset.c`
   covers page math edge cases (0 glyphs, exact multiple, remainder, clamp, wrap).
   *Note:* the table stores generous ranges; presence filtering is Unit 4 (needs the font).

3. **`iconset` core — favourites model + file format (+ tests)** — Favourites as an ordered
   set of `uint32_t` codepoints: add/remove/toggle/contains/count/iterate. Pure
   parse/format: `iconset_favorites_parse(text)` (skip blank/`#` comment lines, accept
   `f31b` / `0xf31b` / `U+F31B`, dedup, ignore garbage) and `iconset_favorites_format(buf)`
   (one lowercase-hex codepoint per line, stable sorted order, optional `# <set>` trailing
   comment via the set table). Round-trip test: format→parse is identity; malformed input
   is ignored not fatal. This is the bake-ready contract (drops into `lv_font_conv -r`).

4. **`icons` mode — grid + preview + presence probing** — Build the screen lazily on first
   activate (menu.c pattern). Create TinyTTF fonts: one per preview size (e.g. 72/48/36/28/
   20) + one grid-cell size, from the configured TTF path; on failure, designed unavailable
   state (dev/claude pattern), return early. Grid: 4-row flex of icon-font labels; fill from
   the current set's present codepoints (probe with the has-glyph API, skip absent), page
   `per_page = rows*cols`. Tap-select with the swipe-vs-tap guard
   (`lv_indev_get_gesture_dir`); selected cell → coral border/press treatment; favourited
   cells → coral marker. Preview column: selected glyph at each size stacked + `U+XXXX`
   (tabular) + set name + page indicator; empty selection → placeholder. All cells/controls
   `LV_OBJ_FLAG_GESTURE_BUBBLE` so shell swipes still work.

5. **`icons` mode — controls + favourites persistence + wiring** — Control column buttons:
   *Pick Set* (advance+wrap set, reset page/selection), *★ only* (toggle the all-sets
   favourites view), *★ fav* (toggle selected in the favourites set + repaint marker),
   *Save* (format → write file → on-screen ok/fail toast), *Prev*/*Next* (page within set,
   clamp). `config.{c,h}`: `KDESKDASH_ICONS_TTF`
   (default `/usr/local/share/kdeskdash/SymbolsNerdFont-Regular.ttf`) and
   `KDESKDASH_ICONS_FAVORITES` (default `/var/lib/kdeskdash/icon-favorites.txt`), env-or
   pattern. On activate, load favourites file if present (missing → empty). Register in
   `main.c` (one `shell_register_content_mode(icons_mode_create("icons", "Icons"))`), add
   the source to `CMakeLists.txt` (mode + core + `test_iconset`). Deploy: `scripts/deploy.sh`
   copies the TTF to `/usr/local/share/kdeskdash/` and ensures `/var/lib/kdeskdash/` exists;
   note both in `deploy/kdeskdash.env.example`.

6. **Build, test, deploy, verify, document** — Host: `cmake --build build` +
   `ctest --test-dir build` (new `test_iconset` green, others unregressed). Cross:
   `cmake --build build-pi --target kdeskdash` + `deploy`. On `rpidash2`: open the mode via
   the menu / `redis-cli set kdeskdash:active_mode icons`; verify a set pages with no empty
   cells, tap→preview works, favourite+save writes the file (`ssh ken@rpidash2 cat
   /var/lib/kdeskdash/icon-favorites.txt`), reopen reloads them; eyeball preview/cell sizes
   and adjust. Capture a hero shot via `scripts/kddss`. Docs: README (new mode row, env vars,
   the favourites-file note), CLAUDE.md (mode + TinyTTF engine note), and a
   `docs/solutions/` entry if the TinyTTF-vs-bake decision earns one.

## Testability

Pure host tests (`test_iconset`) cover: page math (empty/exact/remainder/clamp/wrap), set
table lookups, favourites add/remove/toggle/dedup/order, and favourites parse/format
round-trip incl. malformed input. Everything needing the font (presence probing, rendering,
sizes) is verified on-device in Unit 6 — the only hardware-dependent surface.

## Risks / mitigations

- **Per-render glyph probing cost** — cache the present-codepoint list per (set,page); the
  set/page changes only on button press, not per tick.
- **TinyTTF small-size fidelity** — the preview stack makes any softness visible before an
  icon is adopted; acceptable for a browser (bake later for crisp production use).
- **Favourites write path perms** — deploy pre-creates `/var/lib/kdeskdash`; Save reports
  failure on-screen rather than assuming success.
- **Set table drift vs. the actual TTF** — presence probing (Unit 4) means a too-generous
  range never shows empty cells; a too-narrow range just omits glyphs (caught by eyeball).
