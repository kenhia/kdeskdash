# claude-feed publisher

Publishes Claude Code session activity (hooks) and subscription usage limits
(statusline + a session-free `poll` mode) from each dev machine to the
claude-feed Redis on `rpidash2:6380`, where the dashboard's `claude` mode reads
it. Zero dependencies: one bash script speaking RESP over `/dev/tcp` — no
`redis-cli`, no `jq`. Works on Linux and on Windows under Git Bash (Claude Code
runs hooks/statusline via Git Bash when it is installed).

Contract and rationale: `sprints/007-claude-mode/plan.md`; the multi-source
`claude:limits` contract: `docs/solutions/best-practices/` (sprint 023).

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

## `poll` mode — usage limits with no session running

The statusline only runs while a session is rendering, so on a statusline-only
install the USAGE gauges freeze the moment the last session ends. `poll` mode
refreshes `claude:limits` from whichever session-free source the machine has,
best first:

- **file** — `plan-usage-history.json`, which the Claude **desktop app**
  samples on its own 5-minute timer whether or not any session runs (measured:
  4,638 of 4,660 gaps were exactly 5 min over 27 days). No network, no
  credentials. Percentages only — this file carries no reset timestamps, and
  the script deliberately leaves the `*_resets_at` fields untouched rather
  than writing a sentinel.
- **oauth** — `GET api.anthropic.com/api/oauth/usage` with the CLI's own
  credentials (`~/.claude/.credentials.json`), for headless hosts. Supplies
  reset timestamps too. The `User-Agent: claude-code/<version>` header is
  load-bearing (see the comments in the script); if no CLI version can be
  resolved the call is skipped entirely.

`updated_at` is the **observation** time, not the publish time, and `poll`
reads it back before writing: it never publishes over a fresher observation,
so a live statusline on any host always wins. Run it on a ~5-minute timer —
no faster; that is the desktop app's own cadence against the same endpoint.

### Windows (scheduled task, the cleo install)

Registered from an unelevated PowerShell — `-LogonType S4U` needs elevation,
so the task runs Interactive, and an Interactive console app **always** flashes
a window; `poll-hidden.vbs` is the shim that suppresses it (window style 0,
wait-on-return true so the exit code and time limit still apply). Copy it next
to the script, then:

```powershell
$vbs      = "$env:USERPROFILE\.claude\kdeskdash-pub\poll-hidden.vbs"
$action   = New-ScheduledTaskAction -Execute 'wscript.exe' -Argument "//B //Nologo `"$vbs`""
# RepetitionDuration must be finite: [TimeSpan]::MaxValue is rejected as out of range.
$repeat   = New-ScheduledTaskTrigger -Once -At (Get-Date) `
              -RepetitionInterval (New-TimeSpan -Minutes 5) `
              -RepetitionDuration (New-TimeSpan -Days 3650)
$atlogon  = New-ScheduledTaskTrigger -AtLogOn
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable `
              -ExecutionTimeLimit (New-TimeSpan -Minutes 2)
Register-ScheduledTask -TaskName 'kdeskdash-claude-poll' `
  -Action $action -Trigger $repeat,$atlogon -Settings $settings
```

Two more Windows traps the shim and script already handle, so don't "fix" them
away: `bash` on PATH may be WSL, not Git Bash (the vbs resolves
`%ProgramFiles%\Git\bin\bash.exe` explicitly), and the MSIX-packaged desktop
app redirects `%APPDATA%\Claude\...` into
`%LOCALAPPDATA%\Packages\Claude_<hash>\LocalCache\Roaming\Claude\` for some
processes — `from_file` probes both locations.

### Linux headless (systemd user timer, the kai install)

See `deploy/` conventions; the unit pair is documented with the sprint-023
record. `OnUnitActiveSec=5min`, `Persistent=true`, and lingering enabled so it
survives logout. systemd user units get a minimal PATH — `cli_version()`
probes the usual install locations itself, but verify the first run under the
timer, not just an interactive shell.

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

## Session name (`title`)

Each row on the dashboard is labelled with Claude's own session name. It is in
**no** hook or statusline payload — the hook `session_title` field carries only a
user-set `--name` / `/rename` and is usually empty. The auto-generated name lives
only in the transcript JSONL, as one of two record types:

| record | source |
| --- | --- |
| `{"type":"ai-title","aiTitle":"…"}` | CLI / Code sessions |
| `{"type":"custom-title","customTitle":"…"}` | desktop app auto-name; also CLI `--name` / `/rename` |

Both are rewritten as a session evolves, so `title_from_transcript()` takes the
**last record of either type**. Measured over 55 local transcripts (2026-07-27):
the last title record sits median 3 / p90 15 / max 24 lines from EOF, so the
bounded `tail -n 100` never misses one, and only 1/55 carried both types —
last-in-file settles that case. Sessions with no title at all genuinely have no
such record anywhere in the file (verified), not one beyond the tail window.

Because `transcript_path` is on every hook payload, this yields a name for **all**
sessions — TUI, VS Code, desktop app, headless — where the old statusline-only
`session_name` source covered TUI alone. The statusline still writes `title`; it
simply agrees now.

**The name lags.** Claude generates none for the first few turns, so `title` is
empty early in a session; the dashboard falls back to repeating `project` in a
muted tone. Unlike the `blocked` status there is **no deploy-ordering
constraint** — `title` has always been an accepted hash field, so publisher and
dashboard can land in either order.

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
