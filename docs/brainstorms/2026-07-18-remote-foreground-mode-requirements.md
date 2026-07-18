---
date: 2026-07-18
topic: remote-foreground-mode
codename: R4gnd
---

# kdeskdash `foreground` Mode — Remote Window Foregrounding (VS Code, later Edge)

## Problem Frame

Every observer mode so far (`dev`, `claude`) *watches* the fleet. This mode is different:
it **acts on another machine**. It lists the live VS Code / VS Code Insiders windows running
on `cleo`, and tapping one **brings that window to the foreground on `cleo`** — a physical
control surface on the desk for "jump me to that editor." It is kdeskdash's first control
plane, not just a view, so the trust/security posture matters (see below).

The data pipeline is already built and verified on the other side: **`kvscf`** (a Rust app on
`cleo`) publishes the window list to the shared claude-feed Redis and consumes a focus
command back. This sprint builds only the kdeskdash half — the reader, the renderer, and the
tap→publish action — against a **frozen contract** (kvscf reverse handoff, 2026-07-18;
`.scratch/kvscf-kdeskdash-redis-handoff.md`, full spec `kenhia/kvscf`
`docs/kdeskdash-vscode-mode.md`).

### The contract (frozen, kvscf side live)

Over the existing LAN-reachable **claude-feed Redis** (`rpidash2:6380`, no auth), namespace
`kvscf:*` — *no new endpoint, no new instance*:

- **Instance list** — key `kvscf:instances:<host>` (only `cleo` today), a **String = JSON**,
  TTL 10s, republished ~1s. Discover via `SCAN kvscf:instances:*`. Each instance has:
  `id` (HWND decimal string — the opaque focus token), `label`, `workspace`, `remote`
  (`local|ssh|wsl|devcontainer|codespaces`), `remote_host`, `app`
  (`stable|insiders|exploration|unknown`), `active_file`, `z_index`.
- **Focus command** — pub/sub channel `kvscf:focus:<host>`, **fire-and-forget**, payload
  `{ "token": "...", "id": "...", "maximize": false }`. `token` is a **plaintext shared
  secret** (`KVSCF_TOKEN`, format `kvscf-<64hex>`); kvscf does a plain byte-equality check
  and, if it matches, foregrounds that HWND. **Not a signature** — no HMAC/hashing; the
  token travels verbatim as a field. Redis is open, so the token is the *only* auth.

### Live sample (captured from `192.168.1.144:6380`, 2026-07-18)

The value of `kvscf:instances:cleo` is an **object**, not a bare array:

```json
{"host":"cleo","instances":[
  {"active_file":null,"app":"stable","id":"432410618","label":"ClaudeWorks",
   "remote":"local","remote_host":null,"workspace":"ClaudeWorks","z_index":25},
  {"active_file":"ch3.ipynb","app":"insiders","id":"566170092",
   "label":"gen-ai-langchain (kai)","remote":"ssh","remote_host":"kai",
   "workspace":"gen-ai-langchain","z_index":16}
]}
```

Grounds the parser: root is `{ host, instances:[…] }`; **`active_file` and `remote_host`
are nullable** (null for `local` windows); `z_index` is a **sparse integer** (real Windows
Z-order — values seen 1,8,14,16,20,25,49, not a 0..N sequence). In practice most windows are
**Insiders opened over SSH into `kai`/`kubs0`**, so `remote_host` is display-important. The
actively-focused window (the kdeskdash editor) was `z_index:1`, suggesting **lower =
frontmost** — to confirm with kvscf. kvscf already truncates long `active_file`/`label` with
an ellipsis, so kdeskdash can render them verbatim.

## Layout

Native 1920×440 (physically ~250×58mm → ~7.7 px/mm). claude-mode design language. A fixed
left **app rail** (~25mm ≈ 190px, the control surface) and a **multi-column window grid**
filling the rest.

```text
+--------+----------------------------------------------------------------------------+
|        |  aaa-project   kai      erp-service    kai      mmm-tool     kubs0   ...    |
|  (VS   |  ClaudeWorks   cleo     gen-ai...       kai      nnn-app      kai            |
|  Code  |  ddd-lib       kubs0    kagent          kai      ...                         |
| f0a1e) |  ...            ...     ...                                                  |
|  [▲]   |  (4 columns × 7 rows = 28 windows, alphabetical by label, column-major)     |
|  [▼]   |                                                                             |
+--------+----------------------------------------------------------------------------+
```

Prose is authoritative. **Left rail**: a large app glyph — VS Code (`f0a1e`,
`nf-md-microsoft_visual_studio_code`) via TinyTTF — built as a **1..N app switcher**: one icon
today (no-op with a single app); when `kvscf` later publishes Edge (`f01e9`,
`nf-md-microsoft_edge`), a second icon appears and tapping it filters to that app. Below the
icon(s), **page up/down arrows** appear *only when the window count exceeds one page*.

**Window grid**: a fixed **4-column × 7-row grid = 28 cells** (5 columns / 35 for the future
Edge view — Edge names are shorter and instances more numerous), filled **column-major** so
the **case-insensitive alphabetical-by-`label`** order reads top-to-bottom down each column.
Each cell is a tappable entry sized like a `claude`-mode agent row: **`label`** on top (window
title, coloured by `app`) and **`host`** below (grey). Tapping a cell publishes the focus
command for that window's host. No scrolling — overflow beyond 28 pages via the rail arrows.
A hairline separates rail from grid.

## Requirements

**Feed core (pure, host-tested)**
- R1. A pure module (`kvscf_feed` — no Redis, no LVGL) parses each host's `kvscf:instances`
  JSON String (`{ host, instances:[…] }`; cJSON, already vendored) into a merged array of
  instance records across all discovered hosts, tolerating `null` `active_file`/`remote_host`,
  missing optional fields, and skipping malformed entries. **Sort: case-insensitive
  alphabetical by `label`** (our own order — usually matches Z-order but deterministic and
  the order we want); `z_index` is parsed-but-unused for now. Host-tested with the captured
  live sample.
- R2. Token/host discipline: `<host>` tokens from `kvscf:instances:<host>` keys are validated
  with the same charset contract as `telemetry_host_from_key` before use (they build the
  focus channel name). The instance `id` is treated as an opaque string (never parsed).

**Redis I/O (reuse the 6380 instance)**
- R3. Reads `kvscf:instances:*` (bounded `SCAN` + `GET`) and PUBLISHes `kvscf:focus:<host>`
  on the **existing claude-feed 6380 client** — the same `redis_client_t` command connection
  (`redisCommand`), following `claude_redis.c`. **PUBLISH is an ordinary command**; only
  `SUBSCRIBE` would monopolise a socket into pub/sub mode, and kdeskdash never subscribes. No
  new endpoint and no dedicated pub/sub connection.
- R4. The focus channel is derived **per row** from that instance's `host` field
  (`kvscf:focus:<host>`), never hardcoded to `cleo`, so a second publishing host works with
  no code change.
- R5. Discovery/liveness mirror the `dev`/`claude` cadence: poll ~1–2s (TTL is 10s,
  republished ~1s), discovery `SCAN` a little slower. An endpoint-down state is a designed
  "unavailable" banner (claude/dev pattern); other modes unaffected.

**Focus action**
- R6. Tapping a row publishes `{ token, id, maximize }` to `kvscf:focus:<host>` with the
  token **verbatim** (no signing) and `maximize:false` for the first cut. Tap uses the
  documented swipe-vs-tap guard so a swipe that releases on a row never fires a focus.
- R7. The command is **fire-and-forget** — kvscf sends no ack, so kdeskdash shows *optimistic*
  feedback (a brief "focusing <label>…" on the tapped row) and cannot report success/failure.
- R8. Guard rails: no publish when the token is unset/empty (show a quiet "no token
  configured" state rather than sending an unauthenticated command that kvscf will drop);
  no publish when the endpoint is unavailable.

**Config / security**
- R9. New secret `KVSCF_TOKEN` in `/etc/kdeskdash/kdeskdash.env`, read via the `config.c`
  env pattern. **Trim trailing whitespace/newline/CR** on load — the match is byte-exact and
  the secret can originate CRLF on the Windows side. Never logged; never rendered on screen.
- R10. The kvscf feed reuses the claude-feed endpoint config (same 6380 instance); it needs
  no new host/port env, only the token. (A separate `redis_client_t` vs. sharing the claude
  handle is a plan-time detail; either way it is the same instance and one physical
  connection to 6380 is the intent — R3.)

**Mode / view**
- R11. `foreground` registers as a normal content mode (one `shell_register_content_mode`
  line); swipe/menu integration and the menu tile come free. Tick-driven polling on the UI
  thread (constants at the top of the file, `dev`/`claude` convention).
- R12. The left app rail renders app glyphs via TinyTTF (`LV_USE_TINY_TTF`, already enabled
  for `icons`). A small `app → {glyph, colour}` table (VS Code stable/insiders/exploration;
  Edge later) mirrors `iconset`'s set table. Reuse the `icons.c` load pattern (read TTF bytes
  into a state-owned buffer, `lv_tiny_ttf_create_data`); factor a shared nerd-font helper if
  it falls out cleanly.
- R13. The window grid is a fixed **4×7 = 28-cell** grid (constant `COLS`/`ROWS`; 5×7 reserved
  for Edge), filled **column-major** from the alphabetically-sorted list. Paging math (page
  count, clamp, which slice fills the grid) is pure/host-tested (reuse the `iconset` paging
  approach). Rail up/down arrows page and are shown only on overflow. No scrolling — this
  sidesteps the scroll-vs-swipe-nav conflict; taps use the swipe-vs-tap guard.
- R14. Empty and unavailable states are designed: no instances → "no active editors"
  placeholder; endpoint unreachable → unavailable banner; token unset → "no token" state.

**Visuals**
- R15. claude-mode surface/chrome (surface `#05070d`, panel `#0a0f1a`, hairline `#1b2334`,
  muted `#525d73`, accent coral `#cf6b4a` for the tap/press). **Entry colours mirror the
  `cleo` side** so the two UIs read as one system: the `label` is coloured by `app` —
  **stable/"code" `#60A5EB`** (blue), **insiders `#38BE84`** (green) — and **`host` is
  `#969696`** (grey). `exploration`/`unknown` fall back to ink `#e9edf6` (not in the sample;
  confirm if it appears). Colour is paired with the app glyph on the rail, never the sole
  signal. Coral is the row-press/selected accent, consistent with menu/icons.

## Success Criteria

- Glancing at the panel shows the live VS Code windows on `cleo`, foreground-most first,
  updating within a couple of seconds as windows open/close/reorder.
- Tapping a row brings that exact window to the front on `cleo` (verified live on the kvscf
  side: `PUBLISH kvscf:focus:cleo` with a valid token foregrounds the tapped HWND).
- A wrong/absent token, or an unreachable 6380, degrades to a designed state and never sends
  a bad command or crashes the app; other modes are unaffected.
- Adding Edge later is *data only* — an `app` row in the glyph table plus kvscf publishing
  Edge instances — with the rail becoming a real two-app switcher and no structural change.

## Scope Boundaries

- **VS Code only this sprint.** The feed only carries VS Code today; the rail is
  switcher-ready but shows one icon (tap-to-filter is a no-op with one app). Edge is designed
  for, not built (no Edge data yet).
- **No SUBSCRIBE.** kdeskdash only ever PUBLISHes and reads keys; it never enters pub/sub
  mode (that is kvscf's job on `cleo`).
- **No new Redis endpoint/instance** — reuse 6380 (R3).
- **No maximize toggle** first cut (`maximize:false`); revisit if wanted.
- **No delivery ack** — fire-and-forget; feedback is optimistic (R7).
- **No signing / crypto** — plaintext shared-secret equality is the contract; do not add HMAC.
- **No window management beyond foreground** — no close/open/move; the dashboard foregrounds
  an existing window, nothing more.

## Key Decisions

- **A control-plane mode, deliberately.** Unlike `dev`/`claude`, this mode signals another
  machine. Accepted because the action is narrow (foreground an existing window), gated by
  the shared token, and fire-and-forget.
- **Reuse the 6380 claude-feed connection for read + PUBLISH** (R3): no new endpoint, no
  pub/sub socket; PUBLISH is a normal command.
- **App rail as a 1..N switcher from day one** (R12): the Edge future is a table row, not a
  redesign; today it is a single VS Code icon.
- **Token verbatim, trimmed, never logged** (R6/R9): it is a plaintext secret checked by
  equality — signing would fail the contract; CRLF trimming avoids a silent auth mismatch.
- **Optimistic feedback** (R7): the contract gives no ack, so the UI is honest about that
  (a transient "focusing…", not a success claim).
- **Pure feed core + thin mode** (R1): JSON parsing/sort/validation is host-tested; the LVGL
  layer renders and publishes.

## Dependencies / Assumptions

- The kvscf contract is frozen and live (verified on the kvscf side 2026-07-18). `cleo`
  publishes `kvscf:instances:cleo` and subscribes `kvscf:focus:cleo`.
- `KVSCF_TOKEN` is provisioned identically on `cleo` (kvscf `.env`) and `rpidash2`
  (kdeskdash env); the two must match byte-for-byte.
- The 6380 instance is reachable from the Pi as localhost (already true for `claude` mode);
  its ephemeral posture (32mb, allkeys-lru, no persistence) is fine for small window lists +
  transient commands — TTLs already bound it.
- Single-threaded LVGL: all Redis I/O on the UI/tick thread, short timeouts, lazy reconnect.

## Outstanding Questions

### Resolve Before Planning
- (none blocking — product shape is set: VS Code now, switcher-ready, tap=foreground)

### Resolved (product decisions locked 2026-07-18)
- **Sort**: case-insensitive alphabetical by `label` (our own order; `z_index` unused, so its
  direction no longer matters). **Layout**: fixed 4×7 grid, column-major, rail up/down paging
  on overflow, no scroll. **Row content**: `label` (app-coloured) + `host` (grey) only —
  `active_file`/`workspace`/`remote`/`z_index` not shown. **Colours**: mirror `cleo`
  (stable `#60A5EB`, insiders `#38BE84`, host `#969696`). **Action**: `maximize:false`.
  **Name**: id `foreground`, title "Remote" (codename R4gnd).

### Deferred to Planning
- [Affects R12/render] Which value populates the grey **`host`** line: `remote_host` when set
  (ssh/wsl → `kai`/`kubs0`), falling back to the publisher `host` (`cleo`) or "local" for
  local windows. Proposed: `remote_host ?? host`. Confirm the local-window label ("cleo" vs
  "local") — and whether the label's own "(kai)" suffix should be stripped to avoid
  duplicating `host` (kvscf sets both; likely just show both).
- [Affects R9] Where `KVSCF_TOKEN` is sourced on the Pi (env file line) and confirming the
  systemd `EnvironmentFile` value has no stray CR after the config trim.
- [Affects R13] Cell size at 4×7 on 1920×440 (label + host legibility) — a quick on-device
  eyeball during the build, as with the icons grid.

## Next Steps

→ Plan (units): pure `kvscf_feed` core + tests, 6380 read+PUBLISH additions, config/token,
  the LVGL mode (rail + list + tap→focus), then build on a `feat/foreground-mode` branch.
