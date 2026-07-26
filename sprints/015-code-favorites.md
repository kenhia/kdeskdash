# feat: Remote/Code — favorites that relaunch (dimmed rows + ★/○ markers)

Origin: kvscf sprint 008 contract (`.scratch/kdeskdash-vscode-mode.md` §5).

Ken marks VS Code sessions as favorites so he can close RAM-heavy idle ones without
losing them. A favorite with **no open window** still appears in the Code list —
greyed — and tapping it **relaunches** the editor instead of focusing a window.

**No new key and no new command.** This rides the existing `kvscf:instances:<host>`
feed and the existing `{token, id}` focus command; kvscf decides focus-vs-relaunch
(an HWND parses as an integer, a folder URI never does). Only **ordering**,
**dimming**, and **two new fields** change on our side.

## Contract delta

Every `kvscf:instances:<host>` row now carries:
- `running` — `true` = an actual open window; `false` = a favorite that isn't open.
- `favorite` — `true` if this folder is one of Ken's favorites (open or not).

`running:false` rows are appended to the same `instances` array, with
`active_file`/`z_index` null and — **the important bit** — **`id` is the folder URI,
not an HWND** (e.g. `vscode-remote://ssh-remote%2Bkai/home/ken/src/ai-agents/harness-eval`).
Treat `id` as opaque either way and echo it back verbatim.

### ⚠ Landmine found while reviewing (drives Unit 1)

`KV_ID_MAX` is **24** (sized for an HWND decimal string). Live favorite URIs are
**up to 67+ chars**, so today they would be **silently truncated** and a tap would
publish a broken URI — the relaunch would fail with no visible error. `KV_ID_MAX`
must grow (256) and the focus **payload buffer** must be sized off it, not a fixed
128. (Verified from the live feed; URIs are percent-encoded, so JSON-safe.)

## Design decisions

- **Sort**: `running:true` block first, then the `running:false` block; each block
  case-insensitive alphabetical by label (extends today's label sort).
- **Dim**: non-running rows render muted (label *and* the rotated host tab), since
  they're launchable, not focusable.
- **Marker column**: reserve a small fixed strip on the right of each Code cell (left
  of the rotated host tab) for a nerd-font marker — **★ `U+F04CE`** (md-star, gold)
  on open favorites, **○ `U+F0765`** (md-circle_outline, muted) on non-running
  favorites, blank otherwise. Rendered from a second small TinyTTF font off the
  existing TTF buffer. Edge/Apps views leave the marker blank.
- **Toast**: "launching …" for a non-running tap vs the normal focus toast.

## Units

1. **Pure core + tests** — `KV_ID_MAX` 24 → **256**; add `bool running, favorite` to
   `kvscf_instance_t`; parse both (absent `running` → **true** for publisher
   back-compat, absent `favorite` → false); change `kvscf_sort_by_label` to
   running-block-first, then ci-label. Tests: long URI id round-trips **untruncated**,
   running/favorite parse + defaults, running-first ordering.

2. **kvscf_redis** — size the focus payload buffer from `KV_TOKEN_MAX + KV_ID_MAX + 64`
   so a URI id can never overflow it (silent focus failure today).

3. **Mode** — small marker TinyTTF font in state; a marker label per cell between the
   text label and the host strip; Code fill sets marker + dims non-running rows
   (label/host muted); Edge/Apps clear the marker. Tap toast says "launching" when
   the row isn't running (command itself unchanged).

4. **Verify + ship** — host build + ctest; deploy; screenshot the Code view showing the
   running block, the dimmed favorites block with ○, and ★ on open favorites; confirm
   a tap on a dimmed favorite relaunches on cleo (takes several seconds, then the row
   flips to running with a real HWND). README note; ship.

## Testability

Pure tests cover the truncation landmine (the highest-risk item), field parsing with
defaults, and the running-first sort. The relaunch round-trip is on-device (kvscf side
already verified); rendering/dimming verified by screenshot.
