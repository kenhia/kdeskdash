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
3. Done. Statusline config hot-reloads; hooks apply to sessions started after
   the change (hooks are snapshotted at session start).

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

## Fleet notes (2026-07-03)

- `kai`: installed (Claude Code 2.1.198).
- `cleo`: installed (Git Bash at `C:\Program Files\Git\bin\bash.exe`; script path
  written with forward slashes). Local state lands in `%USERPROFILE%\.claude\kdeskdash-pub\state`.
- `kubs0`: installed (Claude Code 2.1.199 at `~/.local/bin/claude`). Interactive
  sessions run the full lifecycle; headless `claude -p` on 2.1.199 does not
  reliably await SessionEnd hooks at exit, so a `-p` run can leave a session
  hash behind — the dashboard's idle→stale ladder + 2h TTL absorbs it.

## SessionEnd is synchronous by design

Every event publishes fire-and-forget (backgrounded send) except `SessionEnd`,
which sends synchronously and is registered `"async": false`: the CLI process
is exiting, and a backgrounded DEL loses the race with process-group teardown
(ghost session row until the TTL). The hook-level 5s timeout bounds the cost.
