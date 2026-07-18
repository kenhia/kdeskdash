# feat: `foreground` mode — remote window foregrounding (R4gnd)

Origin: docs/brainstorms/2026-07-18-remote-foreground-mode-requirements.md

Goal: a content mode listing `cleo`'s live VS Code / Insiders windows in a 4×7 grid
(alphabetical by label), where a tap publishes a focus command back to `cleo` to bring that
window to the front. Reads + publishes over the existing 6380 data instance. Pure core + thin
mode. kvscf contract is frozen and live (verified 2026-07-18).

## Contract (frozen — see brainstorm for the captured live sample)

- Read: `SCAN kvscf:instances:*` → `GET` each; value is `{ host, instances:[…] }` JSON String,
  TTL 10s. Instance fields: `id` (opaque HWND string), `label`, `host`(publisher),
  `remote_host`(nullable), `app`, `remote`, `workspace`, `active_file`(nullable), `z_index`.
- Write: `PUBLISH kvscf:focus:<host>` with `{ "token": "<verbatim>", "id": "<id>",
  "maximize": false }`. Token is a plaintext shared secret (byte-equality; **not** signed).

## Architecture at a glance

- **`src/kvscf_feed.{c,h}`** — pure, host-tested: JSON parse (`{host,instances[]}`, nullable
  fields), merge across hosts, case-insensitive label sort, the `app → {glyph, colour}` table,
  the grid paging math, and the focus-payload JSON builder. No LVGL/Redis.
- **`src/kvscf_redis.{c,h}`** — 6380 I/O: `SCAN`/`GET` the instance keys and `PUBLISH` the
  focus command, following `claude_redis.c` (`redis_client_t` + `redisCommand`).
- **`src/modes/foreground.{c,h}`** — thin LVGL glue: left rail (VS Code glyph via TinyTTF +
  page arrows) + 4×7 grid + tap→publish. Registered in `main.c`.
- **`src/config.{c,h}`** — new `KVSCF_TOKEN` (trimmed); reuse the claude 6380 endpoint config.

## Decisions carried in from the brainstorm

Sort = ci-alphabetical by `label` (z_index unused); 4×7 column-major grid, rail paging, no
scroll; cell = label (app colour) + host (grey `#969696`); colours stable `#60A5EB` /
insiders `#38BE84`; `maximize:false`; id `foreground`, title "Remote".

## Open items to resolve during the build

- **6380 handle**: recommend `kvscf_redis` opens **its own `redis_client_t`** pointed at the
  claude 6380 endpoint config (`cfg.claude_redis_*`) rather than sharing `claude_redis.c`'s
  static handle. Rationale: matches the documented "each feed its own handle, failure-isolated"
  principle and touches **zero** shipped `claude` code (no regression); the cost is one extra
  localhost connection to the *same instance* (trivial). Still honours the real constraints —
  **no new endpoint, no pub/sub socket** (PUBLISH is an ordinary command). (Alternative:
  refactor the 6380 client into a shared module both feeds use — more churn; defer.)
- **`host` line value**: `remote_host ?? host` (→ `kai`/`kubs0`, else `cleo`/"local");
  confirm local-window label + whether label's "(kai)" suffix duplicates it.
- Cell size / label truncation at 4×7 on 1920×440 — on-device eyeball (icons-grid approach).
- `KVSCF_TOKEN` env line on the Pi; confirm no stray CR survives the config trim.

## Units

1. **`kvscf_feed` core + tests** — Records: `kvscf_instance_t` (id, label, host, remote_host,
   app enum, remote enum, workspace, active_file; keep fields we may show later even if only
   label/host render now). `kvscf_parse(json, out[], max)` handles `{host,instances[]}`,
   JSON `null`, missing fields, malformed→skip; merges multiple hosts into one list.
   `kvscf_sort_by_label` (ci). `app_style(app) → {codepoint, lv_color-ish rgb}` table.
   Paging: reuse/mirror `iconset_page_count`/`clamp` for `PER_PAGE = COLS*ROWS`.
   `kvscf_focus_payload(token, id, maximize, buf)` → compact JSON. `tests/test_kvscf_feed.c`:
   parse the captured sample, null handling, sort, paging, payload string, token trim.

2. **`kvscf_redis` (6380 read + publish) + config** — Own `redis_client_t` init'd from the
   claude 6380 endpoint. `kvscf_redis_discover_step()` (bounded `SCAN kvscf:instances:*`,
   atomic publish, `claude_redis.c` pattern). `kvscf_redis_get_instances(host, buf, max)`
   (`GET` + `kvscf_parse`). `kvscf_redis_focus(host, id, maximize)` → `PUBLISH
   kvscf:focus:<host> <payload>` with the configured token; no-op (return false) if token
   empty or endpoint unavailable. `config.{c,h}`: `KVSCF_TOKEN` via `env_or`, **trim trailing
   whitespace/CR/LF**; never logged.

3. **`foreground` mode — rail + grid + tap** — Lazy `build_screen` (menu/icons pattern). Left
   rail: VS Code glyph (TinyTTF `create_data`, reuse icons.c loader — factor a shared
   `nerdfont` helper if clean) + up/down page buttons (hidden unless overflow), gesture-guarded.
   4×7 grid of cells, column-major fill from the sorted+paged list; each cell = label (app
   colour) + host (grey); tap → `kvscf_redis_focus(...)` + optimistic "focusing <label>…"
   toast (swipe-vs-tap guard). States: empty ("no active editors"), unavailable banner, no
   token. `tick` polls ~1–2s (instances) + slower discovery (`dev`/`claude` constants).

4. **Wire + verify + docs** — `config` fields; register mode in `main.c`
   (`shell_register_content_mode(foreground_mode_create("foreground", "Remote", …))`);
   `CMakeLists.txt` (mode + feed core + redis + `test_kvscf_feed`); deploy env
   (`KVSCF_TOKEN` in `kdeskdash.env.example`, set the real value on the Pi). Host build +
   ctest; cross-compile + deploy; on-device: verify the grid lists `cleo`'s windows, tapping
   one foregrounds it (watch on `cleo`), token/unavailable/empty states, capture a screenshot.
   README (mode row + `KVSCF_TOKEN`), CLAUDE.md (mode + the control-plane note).

## Testability

Pure host tests (`test_kvscf_feed`) cover the risk: JSON parsing incl. `null`/missing/malformed,
cross-host merge, ci-label sort, paging math, focus-payload string, and token trimming.
Redis I/O and rendering are verified on-device in Unit 4; the focus round-trip is confirmed
against the live `cleo` publisher.

## Risks / mitigations

- **Bad/empty token → silent no-focus**: never publish an unauthenticated command; surface a
  "no token" state and skip the publish (R8), so a misconfig is visible, not mysterious.
- **Token CRLF mismatch**: trim on load (Unit 2) — the top failure mode for a byte-equality
  secret sourced from a Windows `.env`.
- **>28 windows**: rail paging (no silent truncation); arrows appear only on overflow.
- **Control-plane blast radius**: the only outbound action is "foreground an existing window",
  gated by the shared token and fire-and-forget; no close/move/open.
