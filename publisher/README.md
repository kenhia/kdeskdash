# claude-feed publisher

Publishes Claude Code session activity (hooks) and subscription usage limits
(statusline) from each dev machine to the claude-feed Redis on `rpidash2:6380`,
where the dashboard's `claude` mode reads it. Zero dependencies: one bash script
speaking RESP over `/dev/tcp` — no `redis-cli`, no `jq`. Works on Linux and on
Windows under Git Bash (Claude Code runs hooks/statusline via Git Bash when it
is installed).

Contract and rationale: `docs/plans/2026-07-02-001-feat-claude-mode-plan.md`.

## Install (per machine, once)

1. Copy `claude-pub.sh` to `~/.claude/kdeskdash-pub/claude-pub.sh` and make it
   executable (`chmod +x`; not needed on Windows).
2. Merge `settings-fragment.json` into user-level `~/.claude/settings.json`,
   fixing the two command paths to that machine's absolute script path
   (forward slashes on Windows, e.g.
   `C:/Users/kenhi/.claude/kdeskdash-pub/claude-pub.sh hook`).
3. Done. Statusline config hot-reloads. Hooks were long assumed to be
   snapshotted at session start, but on 2.1.211 a newly-merged hook fired in a
   session that was **already running** (verified 2026-07-19 on cleo: adding the
   AskUserQuestion hooks mid-session produced a `blocked` publish without a
   restart). Treat pickup as likely-immediate but not guaranteed — restart the
   session if a hook change must take effect.

Requirements: Claude Code ≥ 2.1.80 (statusline `rate_limits`), a Claude.ai
Pro/Max login (API-key auth gets no `rate_limits`; the publisher then skips the
limits hash and the dashboard shows "no data yet").

Override the target instance per machine with `KDD_REDIS_HOST` / `KDD_REDIS_PORT`
in the environment if it ever moves; the default is pinned to the rpidash2 IP so
no DNS is involved.

## Smoke test

```sh
printf '%s' '{"hook_event_name":"SessionStart","session_id":"smoke-1","cwd":"/tmp/smoke"}' \
  | ~/.claude/kdeskdash-pub/claude-pub.sh hook
redis-cli -h 192.168.1.144 -p 6380 hgetall claude:session:$(hostname -s):smoke-1
printf '%s' '{"hook_event_name":"SessionEnd","reason":"other","session_id":"smoke-1","cwd":"/tmp/smoke"}' \
  | ~/.claude/kdeskdash-pub/claude-pub.sh hook   # cleans up + pushes a recent record
```

## Fleet notes (2026-07-03; AskUserQuestion hooks added 2026-07-19)

- `kai`: installed (Claude Code 2.1.198).
- `cleo`: installed (Git Bash at `C:\Program Files\Git\bin\bash.exe`; script path
  written with forward slashes). Local state lands in `%USERPROFILE%\.claude\kdeskdash-pub\state`.
- `kubs0`: installed (Claude Code 2.1.199 at `~/.local/bin/claude`). Interactive
  sessions run the full lifecycle; headless `claude -p` on 2.1.199 does not
  reliably await SessionEnd hooks at exit, so a `-p` run can leave a session
  hash behind — the dashboard's idle→stale ladder + 2h TTL absorbs it.

## Blocked-on-you (AskUserQuestion)

`PreToolUse` and `PostToolUse`, both matched on `AskUserQuestion`, publish
`status blocked` / `status working`: an agent sitting on a question dialog is
hard-blocked on Ken, and would otherwise still read WORKING on the dashboard.
The matcher is re-checked inside the script, so a broader matcher configured
elsewhere cannot mislabel a session.

Verified against Claude Code 2.1.211 (2026-07-19): `AskUserQuestion` does fire
both hooks, `PreToolUse` before the dialog is presented. The `Notification` hook
does **not** fire for it — a matcher-less Notification hook logged nothing — so
there is no fallback trigger. Re-verify on major CLI upgrades. Note that headless
`claude -p` disables `AskUserQuestion` outright, so this can only be exercised
interactively.

The hook payload carries the full question text and the user's answer. The script
reads nothing but `tool_name`; prompt content never reaches Redis.

If the user escapes a question rather than answering, `PostToolUse` may not fire
(it is documented as running on tool *success*). No ghost results: `status` is a
single field every event overwrites, so the next `UserPromptSubmit`/`Stop` clears
it, with the idle→stale ladder as a backstop.

**Upgrade ordering.** The dashboard's `cf_session_from_fields` rejects a session
record whose `status` it does not recognise, so a publisher emitting `blocked` at
a Pi still running an older binary makes those rows *vanish* rather than degrade.
Deploy the dashboard to `rpidash2` first, then merge these hooks on the machines.

## SessionEnd is synchronous by design

Every event publishes fire-and-forget (backgrounded send) except `SessionEnd`,
which sends synchronously and is registered `"async": false`: the CLI process
is exiting, and a backgrounded DEL loses the race with process-group teardown
(ghost session row until the TTL). The hook-level 5s timeout bounds the cost.
