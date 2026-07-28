# 020 — Claude mode: session titles + drop Recent

korg:698 (proposal) · WI #696 (kdeskdash) · WI #697 (k-homelab, separate PR)

## Goal

Make the Claude panel legible at a glance. A row said `kai · kdeskdash ·
Opus 5 · WORKING · 5m` — and when three sessions share a repo, the project name
identifies nothing. Claude already maintains a descriptive name per session
("Build Honorverse star system data scraper"); surfacing it is the whole win.
Pay for the width by dropping the RECENT column, which was the least-looked-at
370px on the panel.

## Where the name actually lives

The finding the proposal rested on, re-confirmed here: the auto-generated name
is in **no hook or statusline payload**. The hook's `session_title` field is
only a user-set `--name` / `/rename`, and is almost always empty. The name
exists solely in the transcript JSONL, as `{"type":"ai-title","aiTitle":…}`
(CLI) or `{"type":"custom-title","customTitle":…}` (desktop auto-name, and CLI
rename). Both get rewritten as a session evolves, so the *last record of either
type* is current.

The publisher already tails transcripts for the model, so this is the same
shape of scrape — and since `transcript_path` rides every hook payload, it
covers TUI, VS Code, desktop and headless sessions alike. The old statusline
`session_name` source only ever saw TUI.

Verified against the local corpus before writing any view code:

- **30/40** recent transcripts yielded a name.
- All **10** misses have zero `aiTitle`/`customTitle` records *anywhere* in the
  file — they are genuinely nameless sessions, not victims of the `tail -n 100`
  window. That was the measurement worth making; it says the bound is free.
- Both record types are present in the corpus (3134 `ai-title`, 6
  `custom-title`), and synthetic transcripts confirm last-of-either-type wins in
  both orders, escapes/quotes/slashes unescape correctly, and empty/absent/
  missing-file inputs all return empty rather than garbage.

## What shipped

**Publisher** (`publisher/claude-pub.sh`) — `title_from_transcript()`, modelled
on the existing `model_from_transcript()`, called from the same post-`case`
enrichment point so every fall-through event (SessionStart, UserPromptSubmit,
Stop, and the AskUserQuestion pair) refreshes the name. It also seeds the
`<sid>.title` state file the SessionEnd recent-record reads, which until now
only TUI sessions ever populated.

**View** (`src/modes/claude.c`) — `proj` becomes a fixed 220px identifier and a
new sky-blue `title` label takes the flex-grow slot; RECENT (`make_recent`,
`render_recent`, `claude_recent_t`, `recents[]`, `recent_empty`,
`CLAUDE_RECENTS`, `ZONE_RECENT_W`, the zone build and the
`claude_redis_get_recent` call) is gone and AGENTS widens 1080 → 1450. USAGE
keeps its hardware-calibrated 470.

## Decisions

**`CPU_SKY` (0x4dabf7) for the title.** The proposal flagged that the obvious
"bright accent" picks both collide with the status column — `GPU_GRASS` reads as
`COLOR_WORKING` green, `VRAM_MANGO` as `COLOR_AWAITING` amber / `COLOR_ACCENT`
coral — so a title in either could be misread as a status. Blue belongs to no
status here. Confirmed on the panel: the title reads as its own channel.

**`montserrat_20`, not 22.** Only 14/20/28/36/48 are compiled into `lv_conf.h`,
and 22 would have baked another font in for a 2px difference. At 20 the title
matches every other row label and the *colour* does the distinguishing, with
`proj` at 28 still the dominant identifier. Easy to revisit if it wants more
weight.

**Column widths.** `status` came down 290 → 240 (`BLOCKED ON YOU` fits with
room) and `proj` is 220, which buys the title ~428px ≈ 39 characters before it
ellipsises — enough for most real names, and `LV_LABEL_LONG_DOT` handles the
rest.

**`CLAUDE_ROWS` stays 5.** Confirmed arithmetic, not assumption: 440px of panel
minus zone padding leaves 392 for content, and a 6th row needs 68 more than the
366 already spent. Dropping RECENT was pure *width* reclaim, exactly as the
proposal predicted — no vertical gain to spend.

**Empty title repeats `project`, muted.** The name lags: Claude generates none
for the first few turns, so a young session has no title. The proposal asked for
a project fallback so the row is never blank; on the panel that does read as a
slight stutter (`k-homelab   k-homelab`), but the muted tone reads clearly as
"no name yet" rather than as a name, and the alternative — a wide hole between
`proj` and `model` — looked more like a bug. Worth revisiting once the fleet
publisher lands and empty titles become rare.

## Verification

Host gate green (16/16). Beyond that, three levels on real data:

1. **Scrape** — against 40 real transcripts and 6 synthetic edge cases (above).
2. **Wire** — a RESP sink captured the hook's actual pipeline, confirming
   `HSET claude:session:kai:… title "kdeskdash Claude Mode session titles"`.
3. **Panel** — deployed to rpidash2 and shot with `kddss`. First with injected
   rows covering short / ellipsising / empty-title / every status; then with the
   new hook run against *this* session's own transcript, which put
   `Start sprint korg:698` on the panel end to end — transcript → hook → Redis →
   1920×440.

Mock keys were cleaned up afterward.

At ship time the same function was pointed at a **Windows** transcript on cleo
(pulled read-only over ssh, without touching that machine's hook config — that
is #697's job) and returned `WI #697 project assignment`. Cross-platform path
handling and the desktop-app `custom-title` record both confirmed on real data.

The README hero image was re-shot, since it still showed the RECENT zone. The
replacement is honest current fleet state and happens to demonstrate both
halves: two scraped titles and one untitled session falling back to its muted
project name.

## Follow-ups

- **WI #697 (k-homelab)** — refresh the vendored `claude-pub.sh` in the
  `claude-hooks` recipe and re-apply on kai/kubs0 (cleo by hand). Until that
  lands, real sessions publish no title and every row shows the muted project
  fallback. No ordering constraint: `title` has always been an accepted hash
  field.
- `claude:recent` is still written on SessionEnd and `claude_redis_get_recent()`
  still exists, both now unread by the view. If RECENT is gone for good, a later
  sprint can drop the write, the reader, and `cf_recent_t`.
- The title is capped at `CF_TITLE_MAX` (96); nothing observed comes close.
