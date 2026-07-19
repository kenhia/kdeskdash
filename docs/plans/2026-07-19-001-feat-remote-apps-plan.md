# feat: Remote mode — Apps (focus-if-running-else-launch)

Origin: kvscf sprint 007 contract (`.scratch/kdeskdash-vscode-mode.md` §4). Third
view in the `foreground` ("Remote") Code/Edge switcher.

## Contract (Apps — `kvscf:apps:<host>`, live on cleo)

- Key `kvscf:apps:<host>` (String=JSON, TTL 10s, ~1s republish); `SCAN kvscf:apps:*`.
- Payload `{ host, ts, apps:[ { key, label, running, id, order } ] }`.
  - `key` — **stable app id** (registry subkey). **This is what the command echoes back**
    (a non-running app has no HWND).
  - `label` — display name. `running` — bool; **non-running rendered greyed**. `id` — HWND
    when running else null (informational, unused). `order` — configured sort index.
- **Command** — same channel `kvscf:focus:<host>`, but keyed by `app`, not `id`:
  `{ "token": "…", "app": "<key>" }`. kvscf does focus-if-running-**else-launch**. A tap on
  either running state sends the same command. `app` takes precedence over `id`.

The one structural new thing vs Code/Edge: the command carries **`app`** (a key), not an
HWND `id` — so a second payload builder + a second publish helper.

## Design decisions

- **Third rail app**: rocket glyph `f14de` (nf-md-rocket_launch) in **red `#ef5350`**;
  tap switches to the Apps view. Rail switcher now Code / Edge / Apps (active bright, inactive
  dimmed — unchanged mechanic).
- **Apps cells**: label in ink when `running`, muted grey when not (the greyed cue). No right
  tab (apps have no host / tab count). Sort by `order`, then label (respects Ken's configured
  order; running/not-running interleaved as configured).
- **Tap** → `{ token, app: key }` to `kvscf:focus:<host>` (launch-or-focus). Same token /
  no-token / unavailable guards as the id-based focus.

## Units

1. **Pure core (kvscf_feed) + tests** — `kvscf_appitem_t { key, label, host, running, order }`
   (KV_APPKEY_MAX). `kvscf_parse_apps_append` ({host,apps[]}; key+label required; `running`
   bool; `order` int). `kvscf_sort_apps` — by `order`, then ci-label, then key. New
   `kvscf_launch_payload(token, app_key, buf)` → `{"token":"…","app":"…"}` (guards empty
   token/key). `tests/test_kvscf_feed.c`: parse (running/not, null id), order sort, launch
   payload.

2. **kvscf_redis** — `kvscf_redis_refresh_apps(kvscf_appitem_t*, max)` (SCAN kvscf:apps:* via
   the shared scan_keys). `kvscf_redis_launch(host, key)` — publish `{token,app}` (mirrors
   kvscf_redis_focus; same token guard, host validation, PUBLISH kvscf:focus:<host>).

3. **Mode (foreground.c)** — add `APP_APPS` to the view enum + third rail icon
   (`make_app_icon`, rocket, red); `update_rail` handles three icons. `refresh()` branch →
   `refresh_apps` into `apps[]`. `fill_cell` branch: label ink(running)/muted(not), empty
   right tab. `cell_cb` branch: Apps → `kvscf_redis_launch(host, key)` (toast "launching/
   focusing <label>"). Empty banner "no apps configured".

4. **Verify + docs + ship** — host build + ctest; deploy; screenshot the Apps view (rocket
   active, running vs greyed, order); confirm a tap launches a not-running app on cleo.
   README (Remote = Code/Edge/Apps). Ship.

## Testability

Pure `test_kvscf_feed` covers apps parse (running/not-running, null id), order sort, and the
`{token,app}` launch payload. The launch round-trip (tap → focus-or-launch on cleo) is
verified on-device; the not-running→launch path is the new behaviour to confirm by tap.
