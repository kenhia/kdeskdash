# feat: `claude` mode — fleet agent activity + usage limits

Origin: docs/brainstorms/2026-07-02-claude-mode-agent-activity-requirements.md
Design sign-off: 1920×440 mockup approved 2026-07-02 (arcs for limits, attention-first
rows, validated dark palette; tokens listed in Unit 5).

## Spike results (all three resolved, 2026-07-02, Claude Code 2.1.198 on `kai`)

**S1 — statusline payload (captured live).** Statusline stdin JSON includes
`rate_limits.five_hour` / `.seven_day` with `used_percentage` (number) and `resets_at`
(unix seconds) — plus useful extras the brainstorm didn't assume: `session_name` (the
session title), `model.display_name`, `workspace.project_dir`, `session_id`, `cwd`.
Consequence: the statusline publisher enriches the session hash (title) in addition
to publishing `claude:limits`. Statusline config hot-reloads (no restart needed).

Update (2026-07-07): the statusline only runs in an interactive terminal (TUI) session —
the desktop app and headless `claude -p` fire hooks but never the statusline, so
`claude:limits` and title/model stopped refreshing for those. The hook path now reads the
model from the transcript (`transcript_path`), so `model` is populated for every session
type; `claude:limits` still comes only from the statusline (the 5h/7d percentages exist
nowhere else), and the dashboard greys + badges the gauges as stale when the snapshot ages.

**S2 — hook payloads (captured live).** All events carry `session_id`, `transcript_path`,
`cwd`, `hook_event_name`. `SessionStart` adds `source` (startup/resume/clear/compact);
`UserPromptSubmit` adds `prompt`/`prompt_id`/`permission_mode`; `Stop` adds
`stop_hook_active`, `last_assistant_message`; `SessionEnd` adds `reason`
(clear/logout/prompt_input_exit/other). **No model/title in hook payloads** — enrichment
comes from the statusline script (S1). Hooks are snapshotted at session start (config
changes affect new sessions only). Headless `claude -p` fires the full lifecycle.

**S3 — fleet survey.**
- `kai`: redis-cli + jq present (but publisher won't need them), 2.1.198, clean settings.
- `cleo`: Claude Code in active use (`~/.claude` populated) but **no settings.json** (clean
  slate), **Git Bash present** (`C:\Program Files\Git\bin\bash.exe`), **no redis-cli/jq**.
  ssh access: `kenhi@cleo`, lands in pwsh.
- `kubs0`: **no Claude Code installed** — publisher install deferred; ship install notes.
- `rpidash2`: Redis 8.0.2 installed; control instance localhost:6379 only; **passwordless
  sudo works** — second instance is straightforward. LAN IP `192.168.1.144`.

Decision driven by S3: the publisher is **one zero-dependency bash script** that speaks
RESP directly over bash's `/dev/tcp` (no redis-cli, no jq). Works identically on Linux and
cleo's Git Bash. JSON field extraction via sed/grep (the needed fields are flat);
`LC_ALL=C` so `${#var}` is byte length for RESP bulk strings. Verify `/dev/tcp` in cleo's
Git Bash during Unit 6 (fallback: ship a `redis-cli.exe` alongside, same script shape).

## Key contract (Redis, instance `rpidash2:6380`)

```
claude:session:<host>:<session_id>   HASH, TTL 2h refreshed on every write
  host, project, cwd, status(working|awaiting), ts(unix s of last lifecycle event),
  started_ts, title?, model?          (model from hook/transcript; title from statusline)
claude:limits                        HASH (account-global, last writer wins)
  five_hour_pct, five_hour_resets_at, seven_day_pct, seven_day_resets_at, updated_at, host
claude:recent                        LIST of compact JSON, LPUSH + LTRIM 20
  {host, project, title?, ended_ts, dur_s?}
```

Rules: hooks own `status`/`ts`/`model` (model read from the transcript); statusline owns
`title`/`claude:limits` and never touches `status`/`ts`. `SessionEnd` DELs the session key and (reason ≠ `clear`) pushes to
`claude:recent`; duration from a local per-session start-stamp file (no Redis read-back —
publisher stays fire-and-forget). Consumer ignores session hashes lacking `status` (guards
the statusline-after-end resurrection race). Host/session tokens validated with the
`telemetry_host_from_key` charset discipline.

## Units

1. **Redis instance (deploy/)** — `deploy/redis-claude.conf` (port 6380, bind
   `0.0.0.0` protected-mode no on trusted LAN, `maxmemory 32mb` +
   `allkeys-lru`, no persistence needed → `save ""`, appendonly no) +
   `deploy/redis-claude.service` (systemd, `Restart=always`). Install over ssh with sudo;
   verify `redis-cli -h 192.168.1.144 -p 6380 ping` from kia.
2. **Publisher (publisher/)** — `claude-pub.sh` with two entry modes:
   `claude-pub.sh hook` (stdin = hook JSON → HSET/EXPIRE/DEL/LPUSH per event) and
   `claude-pub.sh statusline` (stdin = statusline JSON → `claude:limits` + enrichment;
   stdout = one-line statusline `«Model · ctx N% · 5h N% · 7d N%»`). Config block at top
   (KDD_REDIS_HOST/PORT, overridable via env). Backgrounded network write with `exit 0`
   always. Plus `publisher/settings-fragment.json` and `publisher/README.md`.
3. **Config plumbing (src/config.{c,h})** — `KDESKDASH_CLAUDE_REDIS_HOST` (default
   `127.0.0.1`), `_PORT` (6380), `_AUTH` (none), following telemetry config fields.
4. **Pure core (src/claude_feed.{c,h} + tests/test_claude_feed.c)** — no Redis, no LVGL.
   Session record from key + field/value pairs (validate tokens, require `status`);
   display-status derivation (published status + age → WORKING / AWAITING INPUT / IDLE
   ≥15m / STALE ≥40m); attention-first sort (awaiting, working by recency, idle, stale);
   top-N + overflow count; limits snapshot parse incl. `resets_at` → "resets HH:MM" /
   "resets Ddd HH:MM" (localtime) and warn threshold ≥80; relative-age strings (12s/3m/2h).
5. **Feed IO + mode (src/claude_redis.{c,h}, src/modes/claude.{c,h})** — third
   `redis_client_t` on the telemetry.c template (lazy connect, 5s backoff, bounded SCAN
   with atomic list swap); HGETALL per session, `claude:limits`, LRANGE `claude:recent`.
   LVGL view per approved mockup: AGENTS rows (stripe/host/project/status word/age),
   RECENT column, USAGE arc pair (`lv_arc`, 270°) + "as of" line; empty ("no agents
   active"), no-limits ("no data yet"), and unreachable states. Poll ~2s, discover ~5s.
   Tokens: surface #05070d, panel #0a0f1a, hairline #1b2334, ink #e9edf6, secondary
   #8b95ab, muted #525d73, accent #cf6b4a, working #35a271, awaiting/warn #b9832c.
   Register in main.c; add all files to CMakeLists (+ host test).
6. **Fleet install + end-to-end** — kia: replace spike settings with publisher config
   (keep the spike dumps until verified, then remove); cleo: create settings.json +
   copy script over ssh (pwsh quoting; test `/dev/tcp` first); kubs0: notes only (no
   Claude Code yet). Verify: headless session on kia and one on cleo → keys appear on
   6380 with correct fields; limits hash matches statusline.
7. **Build, test, deploy** — host ctest green; aarch64 cross-build; `deploy` target;
   confirm live render on the device; hero screenshot for README when the fleet looks
   busy.

## Requirements coverage

R1–R6 → Units 2, 6 · R7–R8 → Unit 2 · R9–R11 → Units 1, 3 · R12–R15 → Unit 4 ·
R16–R19 → Unit 5 · R20 → Unit 2 (no prompt text published) + Unit 5 (none rendered).

## Risks / watch items

- `/dev/tcp` availability in cleo's Git Bash — checked early in Unit 6; fallback noted.
- Statusline cadence: if it proves chatty, add a cheap min-interval guard (skip write if
  the last publish stamp file is <5s old).
- `SessionEnd` does not fire on hard kill — R14's idle/stale ladder + 2h TTL is the
  designed recovery; thresholds are `#define`s to tune on the device.
- Hooks snapshot at startup: sessions already running when the publisher is installed
  won't publish until restarted — acceptable rollout blip.
