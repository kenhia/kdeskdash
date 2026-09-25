# claude-feed publisher

Publishes Claude Code session activity (hooks) and subscription usage limits
(statusline + a session-free `poll` mode) from each dev machine to the
claude-feed Redis, where the dashboard's `claude` mode reads it. One bash script
plus `kdash-pub` — kdashdata's publisher CLI, which brings the khlenv-resolved
endpoint, CD-12 auth and the key grammar. No `redis-cli`, no `jq`. Works on
Linux, on macOS (BSD userland, `/bin/bash` 3.2 — since 2.4.0) and on Windows
under Git Bash (Claude Code runs hooks/statusline via Git
Bash when it is installed).

**Where it writes is a khlenv stem, not a hostname.** One home:
`KDASH_CLAUDE_REDIS`, which has named the central `rpi53:6379` (authenticated)
since the CD-7 relocation closed out in kdashdata sprint 005. The dual-write
window that carried the move — an unauthenticated `rpidash2:6380` leg alongside
central — is retired along with the old home, and so is `--no-auth`. See
`KDD_LEGS` below.

Contract and rationale: `sprints/007-claude-mode/plan.md`; the generalized
one-key-many-writers pattern:
`docs/solutions/best-practices/independent-writers-need-independent-stamps.md`.

Managed Linux hosts don't install from this directory: `just
publish-publisher` puts a versioned bundle (both publishers, the two poll
units and the Copilot hook template) in the homelab package store as
`artifacts/kdeskdash-publisher/<version>/`, and k-homelab's recipes install
from *that* (see `docs/deploying.md`, "The publisher bundle"). The manual
steps below remain the path for unmanaged machines (cleo).

**Two publishers ship from this directory**, on one version clock. Everything
above and below is about `claude-pub.sh`, the Claude Code feed. Its sibling
`ghcp-pub.sh` does the same job for GitHub Copilot CLI sessions and has its
own section at the end — read it before touching either, because the two
feeds look identical and the Copilot one can say strictly less.

## Install (per machine, once)

0. **`kdash-pub` must already be on the machine** at its fleet path —
   `/usr/local/bin/kdash-pub` on Linux, `C:\tools\bin\kdash-pub.exe` on Windows
   (kdashdata CD-13; installed by kdashdata's `just deploy`, one
   `knarr deploy kdash-pub` across the fleet, cleo included). Without it this
   script publishes nothing and drops a `no-kdash-pub` breadcrumb in its state
   dir. `kdash-pub --version` is the check; `kdash-pub --app kdeskdash endpoint`
   additionally proves khlenv and auth work on that host.
1. Copy `claude-pub.sh` to `~/.claude/kdeskdash-pub/claude-pub.sh` and make it
   executable (`chmod +x`; not needed on Windows).
2. Merge `settings-fragment.json` into user-level `~/.claude/settings.json`,
   fixing the two command paths to that machine's absolute script path.
   **On Windows, name the interpreter explicitly** — a bare `.sh` path is not a
   runnable command there:

   ```
   C:/PROGRA~1/Git/bin/bash.exe C:/Users/kenhi/.claude/kdeskdash-pub/claude-pub.sh hook
   ```

   Read "Windows: why the hook command names `bash.exe`" below before
   shortening that to the script path alone — the short form fails silently and
   unattended.
3. Done. Statusline config hot-reloads. Hooks were long assumed to be
   snapshotted at session start, but on 2.1.211 a newly-merged hook fired in a
   session that was **already running** (verified 2026-07-19 on cleo: adding the
   AskUserQuestion hooks mid-session produced a `blocked` publish without a
   restart). Treat pickup as likely-immediate but not guaranteed — restart the
   session if a hook change must take effect.

Requirements: Claude Code ≥ 2.1.80 (statusline `rate_limits`), a Claude.ai
Pro/Max login (API-key auth gets no `rate_limits`; the publisher then skips the
limits hash and the dashboard shows "no data yet").

Environment overrides:

| var | default | what it does |
|---|---|---|
| `KDD_LEGS` | `claude` | Comma-separated homes to write. `claude` = `--stem KDASH_CLAUDE_REDIS`; `central` = `--stem KDASH_CENTRAL_REDIS`. Both resolve to `rpi53:6379` today and are kept apart deliberately, so the claude family can move again without touching this script. The retired `interim` name is **not** an alias for anything — like any unknown name it publishes nowhere rather than guessing, which is what stops a stale caller resurrecting the dual-write. The first leg is also the one the poll guard reads. |
| `KDD_PUB_BIN` | first of `/usr/local/bin/kdash-pub`, `/c/tools/bin/kdash-pub.exe` | The CLI to exec. Checked for executability however it is chosen, so an override naming a missing file degrades to the breadcrumb rather than failing silently. |
| `KDD_STATE_DIR` | `~/.claude/kdeskdash-pub/state` | Throttle/title state. Exists so the batch-shape test never touches a real install's state. |

No host or port is hardcoded any more: the endpoint comes from khlenv, so moving
a home is a store edit rather than a sweep of every publisher host.

## `poll` mode — usage limits with no session running

The statusline only runs while a session is rendering, so on a statusline-only
install the USAGE gauges freeze the moment the last session ends. `poll` mode
refreshes `claude:limits` from whichever session-free source the machine has.

### Choosing a source, per machine

A decision table, not a recommendation — machines differ:

| Your machine | Path | What you get / give up |
|---|---|---|
| Runs the Claude **desktop app** | **file** — the app's own `plan-usage-history.json` | Percentages only, no network, no credentials, no terms question. No reset stamps, no model-scoped window. |
| Headless with **live Claude Code CLI credentials** | **oauth** — the endpoint the official client polls | Everything: percentages, reset stamps, the model-scoped weekly window. Needs `user:profile` scope and a maintained `~/.claude/.credentials.json`. |
| Neither | statusline only | Gauges update while you work and grey between sessions — which is at least honest now (the panel greys per gauge on the writer's own cadence). |

One machine on the oauth path covers the whole fleet's scoped gauge — the
quota is account-global, so more pollers add freshness, not coverage. The
`.credentials.json` caveat matters: the **CLI** maintains that file, the
desktop app neither writes nor refreshes it, so on a desktop-only machine it
sits expired and the oauth path correctly declines (the fleet's cleo is
exactly this case, hence its scheduled task rides the file path).

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

### The `claude:limits` contract

One hash, several independent writers on different cadences. The rules that
keep them from corrupting each other (the generalized pattern is in
`docs/solutions/best-practices/independent-writers-need-independent-stamps.md`):
stamps are observation times; last observation wins via read-back; a writer
sets only fields it can actually supply, never a sentinel for unknown; and a
field-set whose availability differs by source gets its **own** stamp and its
own read-back guard — otherwise a file-source write (newer, but blind to the
scoped fields) would leave the scoped gauge frozen and looking fresh.

| field | writers | notes |
|---|---|---|
| `five_hour_pct`, `seven_day_pct` | all | percent used, headline windows |
| `five_hour_resets_at`, `seven_day_resets_at` | statusline, oauth | unix s; the file source leaves them untouched |
| `updated_at` | all | **observation** time of the headline set |
| `host` | all | which machine produced the observation |
| `source` | all | `statusline` \| `file` \| `oauth` |
| `expected_refresh_s` | all | writer's cadence (statusline 60, poll 300); the panel greys a gauge past stamp + cadence + 60s grace |
| `scoped_model` | oauth | display string (e.g. `Fable`); the API's model id is null — render it, never match on it |
| `scoped_pct` | oauth | percent used, model-scoped weekly window |
| `scoped_resets_at` | oauth | unix s |
| `scoped_active` | oauth | 1 when this window is the binding constraint |
| `scoped_count` | oauth | `weekly_scoped` entries seen in `limits[]`; ever >1 means the flat fields need to become indexed |
| `scoped_updated_at` | oauth | independent stamp for the scoped set — the load-bearing rule above |
| `scoped_expected_refresh_s` | oauth | cadence for the scoped set |

### Terms of service, plainly

Users forking this deserve to know which line they're standing on. The
statusline and file paths read local first-party data written to your own
disk by software you're licensed to run — no terms surface at all. The oauth
path reads your own quota counter from an undocumented first-party endpoint
with your own client's credentials at the official client's own cadence; the
consumer terms' automated-access clause can be read against any scripted
call, so this is a judgment call each user makes — the practical risk is the
endpoint changing shape, which the publisher treats as "keep the last value",
never as zero. **Browser-cookie scraping is deliberately not implemented**:
tools in this space (the CodexBar lineage this design was researched against)
ship a tier that lifts `sessionKey` from a browser cookie store to call
`claude.ai/api/*`. That is the one method that reads squarely as prohibited
automated access, and a `sessionKey` is a full-account bearer credential —
either reason alone disqualifies it.

### Windows: why the hook command names `bash.exe`

The hook and `statusLine` commands must name the interpreter. A bare,
interpreter-less `.sh` path only runs if the launcher happens to route it
through sh/Git Bash; when Claude Code launches it through PowerShell instead,
PowerShell's native-command fallback hands the `.sh` to ShellExecute, and
because `.sh` has no registered handler Windows pops a modal **"How do you want
to open this file?"** picker instead of running the publisher.

It fails *unattended and silently*: the desktop app re-spawns Claude Code on its
own (`WarmLifecycle:preview`), every warm-up fires SessionStart, and the
transcript still records `hookErrors: []` because the ShellExecute launch
"succeeds". Observed on cleo 2026-07-30 (Claude Desktop 2.1.219): several
identical pickers queued up overnight, and the warm-ups that produced them left
no `.start` file in `state/` while the ones that really ran the hook did.

No single unquoted command string works under all three Windows shells:

| form | sh / Git Bash | PowerShell | cmd.exe |
|---|---|---|---|
| bare `.sh` path | works | **picker dialog** | error |
| fwd-slash `bash.exe <script>` | works | works | error (`/` parsed as a switch) |
| quoted backslash `"...bash.exe" <script>` | works | fails (string literal, needs `&`) | works |
| `.cmd` shim | works | works | error (same `/` issue) |

Forward slashes plus an explicit `bash.exe` covers both launchers actually
observed in play. cmd.exe stays uncovered but fails *loudly* with an error
rather than a modal dialog, which is the acceptable failure mode. `PROGRA~1`
(the 8.3 short path) avoids the space in `Program Files`, so nothing needs
quoting. A `.cmd` shim was prototyped and discarded — it adds a file without
improving coverage.

**Do not "fix" the picker by clicking through it.** Choosing an app in that
dialog registers a permanent user association
(`HKCU\...\Explorer\FileExts\.sh\OpenWithList`). On cleo `.sh` is now bound to
`Code - Insiders.exe`, so a future stray ShellExecute *silently* opens the
script in VS Code instead of prompting — the symptom disappears while the bug
remains.

Linux hosts are unaffected (`.sh` is executable there), and on kai and kubs0
k-homelab's `claude-hooks` recipe owns these entries in any case.

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

One home since the CD-7 close-out, so this is a single check. `kdash-pub` is
what prints the endpoint, so the smoke test cannot disagree with the publisher
about where the feed lives.

```sh
printf '%s' '{"hook_event_name":"SessionStart","session_id":"smoke-1","cwd":"/tmp/smoke"}' \
  | ~/.claude/kdeskdash-pub/claude-pub.sh hook

key=claude:session:$(hostname -s):smoke-1
at() { redis-cli -h "${1%:*}" -p "${1##*:}" "${@:2}"; }   # kdash-pub prints host:port

at "$(kdash-pub --stem KDASH_CLAUDE_REDIS endpoint)" hgetall "$key"   # needs REDISCLI_AUTH

# And the read the poll guard makes, without a socket of your own:
kdash-pub --stem KDASH_CLAUDE_REDIS hget claude:limits updated_at

printf '%s' '{"hook_event_name":"SessionEnd","reason":"other","session_id":"smoke-1","cwd":"/tmp/smoke"}' \
  | ~/.claude/kdeskdash-pub/claude-pub.sh hook   # cleans up + pushes a recent record
```

Neither address is written down here any more — both come from khlenv.

What the script hands to `kdash-pub` is pinned by `publisher/tests/batch-shape.sh`,
which runs in `just check` as `test_publisher_batch`. It stubs the CLI, so it
needs no network and no installed binary.

## Fleet notes (2026-07-03; AskUserQuestion hooks 2026-07-19; poll mode 2026-08-04)

- `kai`: installed (Claude Code 2.1.198). Poll: systemd user timer
  `kdeskdash-claude-poll` on the **oauth** path — the fleet's source of the
  scoped gauge and reset stamps (recorded as k-homelab WI #963).
- `cleo`: installed (Git Bash at `C:\Program Files\Git\bin\bash.exe`; script path
  written with forward slashes). Local state lands in `%USERPROFILE%\.claude\kdeskdash-pub\state`.
  Poll: scheduled task `kdeskdash-claude-poll` on the **file** path (its
  `.credentials.json` is expired and the desktop app never refreshes it).
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

`PreToolUse` and `PostToolUse` fire for **every** tool (see the keepalive
below); on `AskUserQuestion` they publish `status blocked` / `status working`,
because an agent sitting on a question dialog is hard-blocked on Ken and would
otherwise still read WORKING on the dashboard. The script branches on
`tool_name` itself rather than trusting the matcher, so neither behaviour
depends on how the events happen to be registered.

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

## Keeping a working session alive

The dashboard greys a `working` row to IDLE once its `ts` is `CF_IDLE_S` old
(15 minutes, `src/claude_feed.h`), and to STALE at 40. That is right for a
parked session and wrong for a busy one: between `UserPromptSubmit` and `Stop`
a long turn emits no lifecycle event at all, so a session that works for forty
minutes goes grey while it is still cranking (korg #1360).

`PreToolUse`/`PostToolUse` on every tool close that gap. Any tool that is not
`AskUserQuestion` takes a fast path near the top of `hook_mode` which refreshes
`ts`, re-arms the TTL, and exits before the transcript enrichment.

**It writes `ts` and nothing else — never `status`.** A backgrounded Bash call
or a subagent can fire tool hooks while the main agent sits on an
`AskUserQuestion`; a keepalive that wrote `status working` would silently
downgrade a `BLOCKED ON YOU` row to WORKING. Writing only `ts` means it can
make a row fresher but can never change what the row claims. The dashboard is
fine with a bare `ts` update: `cf_session_from_fields` wants `status` and `ts`
in the *hash*, not in any single write.

**Throttled to `KDD_HEARTBEAT_MIN_S` (120s)** by a per-session `<sid>.hb` stamp
in the state dir, the same shape as the statusline's `limits.stamp`. Seven
writes cover a 15-minute window, so one dropped heartbeat can never grey a row.
Without the throttle this would publish on every tool call.

**Cost.** The events are unmatched now, so the script runs once per tool call in
every session on the machine. Throttled out that is a bash start plus three
`sed`s — the fast path is ordered ahead of `cwd`/`project`/`transcript_path`
for exactly this reason. Most noticeable on cleo, where Git Bash process
creation is slow.

**What it does not fix.** No hook fires *during* a tool, so a single very long
tool call — a 25-minute build under `Bash` — still crosses `CF_IDLE_S` and
greys. Closing that would take a background timer, deliberately out of scope:
the keepalive is a balance, not a step toward a daemon.

**`ts` now means "last sign of life", not "last lifecycle event",** and the
row's age readout on the panel changes meaning with it. For a glance device
that is the more useful reading, but it is a real change to what the number
says.

**No ordering constraint**, unlike the `blocked` status above: the keepalive
adds no new field and no new status value, so an older dashboard reads it
correctly. The two halves of the rollout are order-independent too — an
unmatched hook against an old script hits the `AskUserQuestion` re-check and
exits (wasted forks, no wrong data), and a new script under an
`AskUserQuestion` matcher simply never reaches the keepalive.

## Where the hook registration lives

`settings-fragment.json` here is the reference for **unmanaged** hosts (cleo),
installed by hand. On kai and kubs0 the entries are owned by k-homelab's
`claude-hooks` recipe, which reports drift against its own copy of the event
table in `recipes/claude-hooks/settings_merge.py` — so a hand edit to
`settings.json` there is reverted by the next apply, and a matcher change has
to land in both places (korg #1362).

## SessionEnd and Stop are synchronous by design

Every event publishes fire-and-forget (backgrounded `send`) except **`Stop` and
`SessionEnd`**, which use `send_sync`.

`SessionEnd` has always been synchronous, and is registered `"async": false`.
**The reason first given for it — that a backgrounded DEL loses the race with
process-group teardown — is not what was happening**, and WI 2809 measured it:
a `SessionEnd` hook that sleeps 6 s runs to completion, and the `SessionEnd`
branch driven by hand deletes a seeded key correctly. The hook is not killed.

The real hazard is **ordering**. A disowned sender makes a session's events
*concurrent* writers against one key with no ordering between them, so
`SessionEnd`'s DEL can land and then be undone by a straggler queued earlier —
leaving a row reading `working` for the full 2h TTL. Three headless `claude -p`
sessions reproduced it 3 of 3, and the surviving rows named their own cause:
each carried `started_ts` (written only by SessionStart) and `title` (written
only by the enrichment block SessionEnd never reaches).

`Stop` is synchronous for that reason, not for the teardown one. It is the only
straggler close enough to overtake the DEL — in print mode `SessionEnd` follows
it by 13–19 ms — and being backgrounded it was frequently torn down before it
published at all, which is why a finished turn so often failed to reach
`awaiting`. With it synchronous: 0 of 6 sessions left a row behind.

The cost is one extra synchronous write per turn end, paid where nothing is
waiting on it. The interactive events stay fire-and-forget on purpose: a dead
Redis must never slow a Claude session down. The hook-level 5 s timeout bounds
the worst case, and `kdash-pub`'s own is 1.5 s per leg.

---

# GitHub Copilot CLI publisher (`ghcp-pub.sh`)

Publishes GitHub Copilot CLI session activity to `ghcp:session:{host}:{sid}`,
so kxeneon's Agents panel shows Copilot sessions beside Claude ones. Sibling of
`claude-pub.sh`: same `kdash-pub` transport, same stem
(`KDASH_CLAUDE_REDIS` carries both `claude:*` and `ghcp:*` by the registry's
own row), same 2 h TTL, same 2-minute keepalive throttle, same
fire-and-forget-and-exit-0 posture.

Contract: kdashdata `contracts/schemas/ghcp-session.schema.json` (CD-21). The
field names are `claude:session`'s on purpose, so every consumer's display
ladder applies to both families unchanged.

## The event name is an argument

The one shape difference that matters. Claude Code puts `hook_event_name` in
the payload; **Copilot puts the event name nowhere at all**. The only thing
that knows which event fired is the hook declaration that invoked the script,
so it passes the name:

```
ghcp-pub.sh sessionStart      # stdin = the hook payload
```

`ghcp-hooks.json` is the shipped template and wires all five. A declaration
that passed the wrong name would publish one event under another, so the test
reads the names back out of the template and checks each command passes its
own.

## What this feed cannot say, and why

Measured on kai against Copilot CLI **1.0.85** (sprint 038), re-confirming
sprint 012's probe of 1.0.83 — same six events, same order, same units.

The complete user-level hook set is `sessionStart`, `sessionEnd`,
`userPromptSubmitted`, `preToolUse`, `postToolUse`, `errorOccurred`.

- **No turn-end event.** Nothing fires when the agent finishes replying and
  hands control back. So this publisher raises `working` and clears the key,
  and can never emit **`awaiting`**. A Copilot session genuinely waiting on its
  user keeps saying `working` until the reader's freshness ladder ages it
  (fresh → idle at 15 min → stale at 40 min). That window is the feed's known
  blind spot, and it is why the ladder is not optional for this family.
- **`blocked` has no source either**, and sprint 038 measured the approval path
  rather than assuming it. A refused tool fires `preToolUse` and then **no
  `postToolUse` at all** (3 pre / 0 post, measured) — so `preToolUse` fires
  *before* the permission decision and is the last event before a session sits
  on an approval dialog. It is equally the event before every auto-approved
  tool, and carries nothing that separates the two.

  The tempting move is therefore wrong: `claude-pub.sh` can publish `blocked`
  because `AskUserQuestion` is one specific tool whose whole purpose is to block
  on the user. Copilot's block is a property of the **permission system**, not
  of a tool. Emitting `blocked` on `preToolUse` and `working` on `postToolUse`
  would mark every long-running build as blocked — which is the busiest a
  session ever is.

- **No model and no title on any event.** Payloads carry `sessionId`,
  `timestamp` and `cwd`, plus per-event extras (`source`/`initialPrompt` on
  sessionStart, `toolName`/`toolArgs` on tool events, `toolResult` on
  postToolUse, `reason` on sessionEnd). The schema marks both fields normally
  absent and the view falls back to `project`.

  Both values *do* exist on disk — `~/.copilot/session-state/<sid>/events.jsonl`
  has `selectedModel` in its `session.start` record, and `workspace.yaml` has a
  `name`. Reading them is deliberately **not** done here. `name` carries a
  `user_named` flag, and when it is `false` — the default — the "name" is the
  user's raw initial prompt, so publishing it would put prompt text in Redis
  and break the rule `claude-pub.sh` keeps (R20). Anyone wiring these later has
  to gate on `user_named` and should say so where the gate is.

- **A mistyped event name fires nothing and warns nothing.** There is no error,
  no log line, no startup validation. This is why the template's names are
  asserted by a test rather than reviewed by eye.

## Milliseconds — the trap on this feed

Copilot's `timestamp` is in **milliseconds** (measured: `1789622590920`).
Published unconverted it is a stamp about a thousand times further into the
future than now, and because every reader treats a negative age as clock skew
and therefore fresh, such a record reads as permanently, convincingly live —
a session that ended weeks ago still showing green, with nothing anywhere
reporting an error.

`ts_of()` divides, and it decides by the **schema's own bound** (`1e11` seconds
is the year 5138; every millisecond stamp after 1973 exceeds it) rather than by
counting digits, so it stays correct if Copilot ever changes the unit. A stamp
that is missing or still out of range falls back to the local clock — a worse
answer than the payload's, and a far better one than a record the schema
rejects silently.

## `userPromptSubmitted` fires *before* `sessionStart`

Reproducibly, by ~5 ms, in `-p` mode — on both 1.0.83 and 1.0.85. So
`sessionStart` is **not** "the first write". `started_ts` is therefore taken
from the sessionStart payload's own stamp and written only by that event;
`hset` merges fields, so the record converges correctly whichever order the two
arrive in.

## What it does not write

**No `ghcp:recent`.** The registry keeps that key optional and *unschema'd*.
A publisher inventing it would be minting a contract this repo does not own, so
`sessionEnd` is a bare `DEL` and nothing else.

**No `errorOccurred` handling.** It is a real event and deliberately unwired:
there is no status this feed could honestly publish from it. Silence beats a
guess.

## Install

Copilot hooks are **user-level JSON files**, one per concern, in
`~/.copilot/hooks/`. There is no `settings.json` to merge into — which is why
`ghcp-hooks.json` ships in the bundle as a deliverable rather than as a
reference fragment. Drop it in as `~/.copilot/hooks/kdeskdash-ghcp.json` and
fix the paths to the machine's own.

**macOS gets its own file**, `ghcp-hooks.darwin.json`: the same five events
with the publisher at `/Users/ken/...` instead of `/home/ken/...`. A hook
command is not `$HOME`-expanded and k-homelab installs the file byte-identical,
so a host whose `$HOME` differs needs a shipped file of its own rather than a
rewrite. `docs/deploying.md` has the platform table; the test asserts the two
files differ only in the path.

Files in that directory are independent: this one sits beside any others (kai
has a `klams-sync.json`) and neither knows about the other.

**No comment keys in that file.** `claude-pub.sh`'s `settings-fragment.json`
carries `"//"` notes because Claude Code tolerates them. Copilot's hook file is
not known to, and the failure mode here is the bad one — an unparseable hook
file would disable every hook in it silently, exactly like a mistyped event
name. Documentation lives in this README instead.

On Windows, name the interpreter explicitly with forward slashes, for the same
reason as the Claude hook (see "Windows: why the hook command names
`bash.exe`"); the template's `powershell` variant already does.

Managed hosts get this from k-homelab's `copilot-hooks` recipe (korg WI 2754),
which installs from the store bundle. cleo installs by hand.

## Smoke test

`kdash-pub` is a publisher: it has `hget` but no `keys` and no `hgetall`, so
you need the session id rather than a scan. `copilot --session-id <uuid>` sets
it for a new session, which is what makes this a one-liner:

```bash
SID=$(uuidgen)
copilot --session-id "$SID" -p 'run: sleep 40; echo hi' --allow-all-tools &

# While it runs — status is `working`, ts is a TEN-digit number:
P="kdash-pub --app kdeskdash --stem KDASH_CLAUDE_REDIS"
$P hget "ghcp:session:$(hostname -s):$SID" status
$P hget "ghcp:session:$(hostname -s):$SID" ts

wait
# After it exits — sessionEnd DELs the key, so both print nothing:
$P hget "ghcp:session:$(hostname -s):$SID" status
```

**`ts` must be 10 digits.** Thirteen means the millisecond conversion is broken
and the row will read as live forever. An empty `hget` is an answer, not an
error — it means the key (or the field) is absent.
