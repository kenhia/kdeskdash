# feat: Remote mode — Edge windows + Code/Edge rail switcher

Origin: kvscf WI #474 reverse handoff (`.scratch/kdeskdash-vscode-mode.md` §3;
klams `019f777f-34be-7c71-a387-ea0ff94e37ea`). Extends the `foreground`
("Remote") mode built in `docs/brainstorms/2026-07-18-remote-foreground-mode-requirements.md`,
which was designed switcher-ready for exactly this.

## Contract (Edge — frozen, live on `kvscf:edge:cleo`, 27 windows / 9 named)

- Key `kvscf:edge:<host>` (String=JSON, TTL 10s, ~1s republish); `SCAN kvscf:edge:*`.
- Payload `{ host, ts, windows:[ { id, label, named, tab_count, z_index } ] }`.
  - `id` — HWND string, the focus token (same as VS Code).
  - `label` — ready to render (named → the user name; unnamed → active tab title).
  - `named` — bool; `tab_count` — int for unnamed, **`null` for named**; `z_index` — enum order.
- **Focus is identical**: `PUBLISH kvscf:focus:<host> {token,id,maximize}` — `id` is a
  kind-agnostic HWND. `kvscf_redis_focus()` is reused verbatim.

## Design decisions (confirmed 2026-07-18)

- **Code/Edge switcher**: the rail gains a second app glyph (Edge `f01e9` under VS Code
  `f0a1e`); tapping switches which source fills the grid. Active icon full-colour, **inactive
  dimmed**. Default view: VS Code.
- **Edge cells**: label coloured **teal `#2ec4c4`** when `named`, muted grey when unnamed.
  Ordering: **named first (alphabetical), then unnamed (alphabetical)** — the teal/muted split
  reads the grouping, no separator.
- **Right tab**: VS Code keeps its rotated `remote_host` tab; Edge has no per-window host
  (all local to the publisher), so its right tab shows **`tab_count`** instead — rendered
  **whenever non-null** (today unnamed only; auto-appears for named if kvscf later populates
  it). [kvscf follow-up: populate `tab_count` for named windows too — "spring cleaning" cue.]
- Focus, token, endpoint, empty/unavailable/no-token states: unchanged and shared.

## Units

1. **Pure core (kvscf_feed) + tests** — `kvscf_edge_t { id, label, host, named, tab_count
   (-1 = null), z_index }`. `kvscf_parse_edge_append(json,…)` parses `{host, windows[]}`
   (host validated, id/label required, `tab_count` null→-1). `kvscf_sort_edge` — named block
   (ci-alphabetical) then unnamed block (ci-alphabetical). Reuse `kvscf_page_*`.
   `tests/test_kvscf_feed.c`: parse (named/unnamed, null tab_count), sort grouping, cap.

2. **kvscf_redis** — `kvscf_redis_refresh_edge(kvscf_edge_t *out, int max)`: `SCAN
   kvscf:edge:*` + `GET` + parse + sort, mirroring `kvscf_redis_refresh`. Same handle;
   `kvscf_redis_focus` unchanged.

3. **Mode (foreground.c)** — `app` state (CODE|EDGE). Rail: two glyph labels (VS Code, Edge)
   in an app-switch row, each a tap target (gesture-guarded) that sets `app`, resets page,
   refreshes; active icon full-colour, inactive dimmed (opacity/grey). `refresh()` branches:
   CODE→`kvscf_redis_refresh` into `items[]`; EDGE→`kvscf_redis_refresh_edge` into `edge[]`.
   `paint_cell` branches: CODE unchanged (app-colour label + rotated host tab); EDGE
   teal/muted label + rotated `tab_count` tab (blank when null). Focus tap: both call
   `kvscf_redis_focus(host, id, false)` with the row's host/id.

4. **Verify + docs + ship** — host build + ctest; cross-compile + deploy; on-device
   screenshot **both** views, confirm the switcher dims/highlights, Edge grouping/colours, and
   that tapping an Edge window foregrounds it on cleo. README (Remote = Code/Edge), then ship.

## Testability

Pure `test_kvscf_feed` covers the Edge parse (incl. null `tab_count`) and the named-first
sort. Rendering/switcher/focus verified on-device (Unit 4); the Edge focus round-trip is the
same command already verified for VS Code.
