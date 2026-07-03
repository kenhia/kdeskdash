---
date: 2026-07-02
topic: claude-mode-agent-activity
---

# kdeskdash `claude` Mode — Fleet Agent Activity + Usage Limits

## Problem Frame

The dashboard shows machine load (`dev`) but nothing about what the **Claude Code agents**
across the fleet (`cleo` [Windows], `kai`, `kubs0` [Linux]) are doing. The `claude` mode is
an at-a-glance answer to two questions: *"which agents are working / waiting on me right
now?"* and *"how much of my subscription limits have I used?"* (the numbers currently only
visible in the Claude app under Settings → Usage).

Unlike `dev` mode — which was "mostly a view" over kpidash's existing pipeline — this
feature must build its **own pipeline end to end**:

1. **Publishers** on every machine: Claude Code *hooks* (session lifecycle) and a
   *statusline script* (usage limits) publishing small JSON blobs to Redis. Both are
   configured once per machine in user-level `~/.claude/settings.json`, so they cover every
   project and every session — terminal CLI **and** the VS Code Claude Code window, which
   shares the same `~/.claude` directory and fires hooks identically.
2. **A second Redis instance on `rpidash2`** (the dashboard's own host). Deliberately *not*
   on `rpi53`: the telemetry Pi has been dropping its network, and agent activity should not
   share that point of failure. For kdeskdash the new feed is a **localhost read** — the
   most reliable dependency possible.
3. **The mode itself**: a pure, host-testable feed core + a thin LVGL view, following the
   `telemetry.c` second-endpoint template and the pure-core/thin-mode convention.

Research findings that shape the design (verified against docs + community tooling,
2026-07-02, Claude Code 2.1.198 installed locally):

- **Hooks** (`SessionStart`, `UserPromptSubmit`, `Stop`, `SessionEnd`) receive JSON on
  stdin (`session_id`, `cwd`, model, session title) and support `"async": true` + short
  timeouts, so a slow/unreachable Redis can never block a session. Hook payloads do **not**
  include the hostname (script adds it) and do **not** include usage limits.
- **Usage limits ARE machine-readable** — the statusline input JSON includes
  `rate_limits.five_hour` / `rate_limits.seven_day` (`used_percentage`, `resets_at`) for
  Pro/Max subscribers on Claude Code ≥ 2.1.80. This is the same supported surface all the
  community statusline projects use. The alternative (`api.anthropic.com/api/oauth/usage`)
  is undocumented and currently 429-prone — rejected. Consequence: **limits update only
  while some session is running somewhere**, so the panel shows an "as of" age.
- **OpenTelemetry** exports token/cost metrics but no session status and no limit
  utilization — not a fit; hooks + statusline need no collector infrastructure.

## Layout

Native 1920×440. Three zones: sessions (the workhorse, widest), recent completions, and
usage gauges. Coral is the mode's brand accent (Claude); status colors are separate and
always paired with a status word (never color-alone).

```text
+------------------------------------------------+--------------------+---------------------+
| AGENTS                                   3 live| RECENT             | USAGE               |
|                                                |                    |                     |
| | kia    kdeskdash        WORKING         12s  | golz-machetes      |   ( 42% )  ( 71% )  |
| | cleo   worklog          AWAITING INPUT   3m  |  kia - 2h          |    5 HR     7 DAY   |
| | kubs0  kpidash          WORKING         45s  | kpidash            |   resets    resets  |
| | kia    korg             IDLE            18m  |  kubs0 - 5h        |   14:00    Tue 07:00|
| | cleo   dotfiles         STALE           41m  | dotfiles           |                     |
|                                                |  cleo - yesterday  |  as of 12s ago      |
+------------------------------------------------+--------------------+---------------------+
```

Prose is authoritative: sessions zone is a vertical list of rows (status stripe + host +
project + status word + age, age right-aligned, tabular figures); recent zone is a compact
list (project, host, relative completion time); usage zone is two arc gauges (LVGL `lv_arc`)
with the utilization numeral centered, window label + reset time beneath, and a small
"as of" freshness line. A hairline separates zones.

## Requirements

**Publisher — session activity (per machine)**
- R1. A hook script publishes session state to the claude-feed Redis on these events:
  `SessionStart` (status *working*), `UserPromptSubmit` (*working*), `Stop` (*awaiting
  input*), `SessionEnd` (remove/complete). Configured in user-level `~/.claude/settings.json`
  so it covers all projects, terminal and VS Code sessions alike.
- R2. Hooks are registered `"async": true` with a short timeout (≤5s) and always exit 0 —
  publishing failures are silent and can never block or annoy a Claude session.
- R3. Each session is one Redis hash `claude:session:<host>:<session_id>` (host added by
  the script, e.g. `$(hostname)`) with fields: `host`, `project` (cwd basename), `cwd`,
  `status`, `title` (session title if present), `model`, `ts` (unix seconds of last event).
  Every write refreshes a TTL (~2h) so crashed/killed sessions age out on their own.
- R4. On `SessionEnd` the script also `LPUSH`es a compact completion record onto
  `claude:recent` and `LTRIM`s it (keep ~20): `host`, `project`, `title`, `ended_ts`,
  session duration if cheaply derivable.
- R5. The publisher is a POSIX shell script (target: Linux + Git Bash on `cleo`), with no
  dependencies beyond `redis-cli` and `jq` (or a self-contained equivalent if `cleo` makes
  those awkward — resolved by the cleo spike, see Outstanding Questions).
- R6. Publisher scripts + settings fragments + install notes live in this repo under
  `agents-pub/` (name TBD in planning) — kdeskdash owns the key contract, so the publisher
  that writes it lives here too (not in kpidash).

**Publisher — usage limits (per machine)**
- R7. A statusline script (`statusLine` in user-level settings) reads the statusline JSON
  from stdin and publishes `rate_limits` to a single hash `claude:limits`: `five_hour_pct`,
  `five_hour_resets_at`, `seven_day_pct`, `seven_day_resets_at`, `updated_at`. Last writer
  wins — the numbers are account-global, so all machines agree.
- R8. The statusline script stays fast (it runs on every statusline refresh): fire-and-forget
  write, silent on failure, and it emits a normal one-line statusline text so it earns its
  place (e.g. model + 5h/7d percentages).

**Redis endpoint (`rpidash2`)**
- R9. A second Redis instance runs on `rpidash2` port **6380**, reachable from the LAN,
  holding only the `claude:*` namespace. The existing control Redis (127.0.0.1:6379) stays
  localhost-only — exposing mode-control keys to the LAN is the reason we don't reuse it.
  Instance config (systemd unit / conf) ships in `deploy/`.
- R10. kdeskdash reads the feed via a third `redis_client_t` handle following the
  `telemetry.c` template (lazy connect, 5s backoff, short timeouts), configured by
  `KDESKDASH_CLAUDE_REDIS_HOST` / `_PORT` / `_AUTH` with defaults `127.0.0.1:6380`.
  Publishers should target the IP or an `/etc/hosts` pin (the 250ms connect timeout does
  not bound DNS — `src/redis.h` caveat; moot on the dashboard side, which reads localhost).
- R11. The `rpi53` telemetry path is untouched: `dev` mode keeps reading `rpi53`, and a
  down/flaky `rpi53` has zero effect on `claude` mode (and vice versa).

**Mode — feed core**
- R12. A pure module (`claude_feed` — no Redis, no LVGL, host-tested) owns parsing and
  presentation state: session records, status derivation, staleness, sort order, and the
  limits snapshot. The LVGL layer renders what the core hands it.
- R13. Discovery via bounded `SCAN claude:session:*` batches with atomic list publication
  (the `telemetry_discover_step` pattern); host/session tokens validated with the same
  charset discipline as `telemetry_host_from_key`.
- R14. Displayed status is derived from last event + age: *working* and *awaiting input*
  as published; a session with no update for a threshold (~15 min, tunable) displays as
  *idle*; well beyond that (~40 min) as *stale* (likely killed); TTL expiry removes it.
  `SessionEnd` moves it to recent.
- R15. Sort: attention first — *awaiting input*, then *working* (most recent first), then
  idle/stale. More sessions than fit → show the top rows plus a "+N more" indicator (no
  scrolling; this is a glanceable appliance, not a terminal).

**Mode — view**
- R16. `claude` registers as a normal content mode (one `shell_register_content_mode` line);
  swipe/menu integration comes free. Tick-driven polling on the UI thread: sessions+limits
  every ~2s, discovery every ~5s (constants at top of the mode file, `dev` convention).
- R17. Usage zone: two arc gauges (5h, 7d) with percentage numeral, reset time rendered as
  local wall-clock ("resets 14:00" / "resets Tue 07:00"), and an "as of <age>" freshness
  line. Arc + numeral shift to the warning treatment at high utilization (threshold ~80%).
  If `claude:limits` is absent (no session has ever published), the zone shows a quiet
  placeholder, not zeros.
- R18. Empty and unreachable states are designed, not accidental: no sessions → "no agents
  active" placeholder (recent + usage still shown); feed Redis unreachable → an unavailable
  banner in the same spirit as `dev` mode's, other modes unaffected.
- R19. Visuals use the validated dark palette (surface `#05070d`; accent coral `#cf6b4a`;
  status: working `#35a271`, awaiting `#b9832c`, idle/stale muted gray) — validated with the
  dataviz six-checks against the dark surface (CVD ΔE 13.0 worst pair, all ≥3:1 contrast).
  Status is always dot/stripe + word, never color alone. Ages/percentages use tabular
  figures. The bar for the hero screenshot: this mode should be README-front-page quality.

**Privacy**
- R20. The dashboard shows project/dir names and session titles only — never prompt text or
  message content. (It sits on a desk in view of anyone walking by.)

## Success Criteria

- Glancing at the panel answers "is anything waiting on me?" in under a second — an
  *awaiting input* session is the most visually prominent thing on screen.
- A session started anywhere in the fleet (terminal or VS Code, any machine) appears within
  a few seconds; killing a terminal doesn't leave a permanent ghost (ages to stale, expires).
- The usage arcs match the Claude app's Settings → Usage numbers, with honest freshness.
- `rpi53` being down does not degrade `claude` mode at all.
- The mode produces the README hero screenshot without staging tricks.

## Scope Boundaries

- **No transcript parsing** — the JSONL format is explicitly unstable; "what the agent did"
  is represented by project + title + duration only.
- **No prompt/message content on screen** (R20) and none published to Redis.
- **No control plane** — the dashboard observes sessions; it cannot signal or stop them.
- **No token/cost analytics** — no OTel collector, no cost dashboards; limits arcs only.
- **No subagent-level detail** — one row per top-level session this sprint; `SubagentStart`/
  `SubagentStop` enrichment is a possible follow-up.
- **No history persistence** — live view + `claude:recent` list only.
- macOS publisher support: nothing precludes it, but it's untested/out of scope (no Mac in
  the fleet).

## Key Decisions

- **Hooks + statusline, not OpenTelemetry**: OTel has no session-status or limit metrics and
  needs a collector; hooks/statusline are supported surfaces with zero extra infrastructure.
- **Statusline JSON for limits, not the OAuth endpoint**: documented-and-supported beats
  undocumented-and-429-prone; accepted cost is freshness tied to session activity ("as of"
  line makes that honest).
- **Second Redis instance on rpidash2 (port 6380), not reuse of the control instance**:
  reusing would mean exposing mode-control keys to the LAN; a dedicated instance is a
  five-line systemd drop-in and keeps the control plane localhost-only. Not on `rpi53` by
  design (flaky NIC; independent failure domains).
- **Publisher lives in this repo** (R6): the consumer defines the key contract; keeping
  writer and reader in one repo keeps them honest. kpidash stays a metrics project.
- **Per-session hash + TTL, completions list**: self-healing (crashed sessions expire),
  O(1) writes, and a bounded recent list — no unbounded growth, no cleanup job.
- **Status derived from last event + age** (R14): hooks can't report "process killed", so
  the view must degrade gracefully through idle → stale → gone rather than trust the last
  written status forever.
- **Arcs for limits**: two independent quotas, not compared series — gauges with a numeral
  read instantly and `lv_arc` is a native widget (cheap, and photogenic for the hero shot).

## Dependencies / Assumptions

- Claude.ai Pro/Max subscription (statusline `rate_limits` is absent for API-key auth) and
  Claude Code ≥ 2.1.80 on all machines (`kai` verified at 2.1.198).
- No existing user-level statusline or hooks to merge with on `kai` (verified — clean
  slate); assumed same on `cleo`/`kubs0`, checked during rollout.
- `rpidash2` is reachable from all three machines; port 6380 open on its firewall (if any).
- The VS Code Claude Code window shares `~/.claude` settings/hooks with the CLI (per docs;
  re-verified trivially during the spike by watching keys appear from a VS Code session).
- Single-threaded LVGL posture unchanged: all feed I/O on the UI thread via `tick`, short
  timeouts, lazy reconnect.

## Outstanding Questions

### Resolve Before Planning
- (none — product decisions are resolved)

### Deferred to Planning (spikes first, in this order)
- [Affects R7][Needs research] **Spike 1 — statusline payload shape**: configure a
  statusline script on `kai` that dumps its stdin JSON to a file; confirm the exact
  `rate_limits` field names/types on 2.1.198 before freezing the publisher. (Community
  sources agree on the shape but it's version-dependent; verify, don't trust.)
- [Affects R1/R3][Needs research] **Spike 2 — hook payload shape**: same dump-to-file trick
  for `SessionStart`/`Stop`/`SessionEnd`; confirm `session_id`, `cwd`, title/model fields,
  and whether `SessionEnd` reliably fires on terminal kill vs only on clean exit (drives the
  R14 staleness thresholds).
- [Affects R5][Needs research] **Spike 3 — cleo publisher**: confirm Git Bash presence and
  whether `redis-cli`/`jq` are workable there; fallback is a tiny self-contained publisher
  (single PowerShell script speaking RESP over TCP, or a static redis-cli binary).
- [Affects R3][Technical] Session title: which hook events carry it and is it stable across
  a session? If unreliable, rows show project only (title becomes best-effort).
- [Affects R9][Technical] Redis instance hardening: bind address, `requirepass` or not on a
  home LAN, `maxmemory` + eviction for the `claude:*` namespace. Decide in planning.
- [Affects R17][Technical] `resets_at` timezone handling on the Pi (unix ts → local wall
  clock; DST via TZ env in the service unit).

## Next Steps

→ Design mockup (1920×440, LVGL-achievable) for review — the hero-screenshot bar makes
  visual sign-off worth doing before any LVGL code.
→ Then `/ce-plan` for structured implementation planning (spikes 1–3 first).
