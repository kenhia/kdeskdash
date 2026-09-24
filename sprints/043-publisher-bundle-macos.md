# Sprint 043 — The publisher bundle works on macOS

**Proposal:** korg:3172 (slice 6.5 of program korg:3148, "kimac: the fleet's
first Mac, fully onboarded").
**Covers:** WI 3170 (claude-pub.sh publishes nothing on macOS; ghcp-hooks.json
bakes `/home/ken`).
**Branch:** `043-publisher-bundle-macos`, off `050b544`.

Overseen sprint: run as karc leg `kdeskdash-8c3637` on kai, reviewed on the
proposal thread, shipped on the overseer's green light.

## Goal

k-homelab sprint 078 (korg:3142, handoff korg:3171) installed the pinned
publisher bundle `2.3.0-69cad0d` on kimac and found the feed dead there: every
Claude hook exited silently having published nothing, and the Copilot hook file
could not be installed at all. Make the bundle work on a Mac, publish it, and
prove it with WI 3170's harness on kimac — without moving the Linux feed.

## Premise check

Both halves held, measured in this tree:

- `jstr()` in `claude-pub.sh` was the sed BRE with `\|` alternation the WI
  quotes; `ghcp-pub.sh`'s `jstr` is the portable `grep -oE` form.
- `date -d` sat in `first_turn_ts` and `iso2epoch`, `stat -c %Y` in
  `cli_version`, both commented as degrading silently.
- `ghcp-hooks.json` carries `/home/ken/.copilot/kdeskdash-pub/ghcp-pub.sh` in
  every `bash` command.

## The Copilot half — a per-platform file

The overseer's recommendation was a `ghcp-hooks.darwin.json` in the bundle,
with the instruction to stop if kdeskdash's contract docs argued otherwise.
They do not: `publisher/README.md` says only "fix the paths to the machine's
own" (the manual-install path), and `docs/deploying.md` says the template is a
deliverable. Nothing here claims one file per bundle.

k-homelab's side was read before building (`recipes/copilot-hooks/apply.sh`,
through kaed): `hook_paths_match` checks **only** each entry's `bash` field,
against `$HOME/.copilot/kdeskdash-pub/ghcp-pub.sh `. So the darwin file:

- is the Linux one with `/home/ken/` → `/Users/ken/` in the `bash` commands;
- drops the `powershell` variant, which a Mac never reads and which would
  otherwise carry a `C:/Users/kenhi` path onto a Mac for no reason.

`ghcp-batch-shape.sh` §16 derives the expected darwin file from the Linux one
(drop `powershell` lines, re-home the path) and compares it whole. A sixth event
added to one template and not the other fails there, which matters because a
mistyped Copilot event fires nothing and warns nothing. Negative control: a
darwin file with one event renamed fails the assertion.

**For k-homelab (slice 9, korg:3143):** a host whose `uname -s` is `Darwin`
installs `ghcp-hooks.darwin.json` as `~/.copilot/hooks/kdeskdash-ghcp.json`;
every other host keeps `ghcp-hooks.json`. The table is in `docs/deploying.md`.

## The Claude half — portable parsing

- **`jstr`** now uses `ghcp-pub.sh`'s `grep -oE` form. One behaviour change,
  and it is a fix: the sed form began with a greedy `.*`, so it returned the
  **last** occurrence of a field name. Claude Code emits its own fields before
  `tool_input`, whose contents are arbitrary, so a nested `"cwd"` later in the
  payload shadowed the real one — on Linux too. `batch-shape.sh` §13 pins
  first-match, and against the pre-sprint script it fails with
  `project cargo cwd /tmp/cargo`, so the old bug was real, not theoretical.
  `oauth_token` already bounded its input so that "last" and "first" agree.
- **`iso_epoch`** is one helper for both date parses: GNU `date -u -d` first,
  then BSD `date -j -u -f '%Y-%m-%dT%H:%M:%S%z'`, after stripping fractional
  seconds and closing up a `+HH:MM` offset (BSD's `%z` reads `+HHMM` only).
  `first_turn_ts` and `iso2epoch` both call it.
- **`mtime`** is `stat -c %Y`, then `stat -f %m`. `cli_version` uses it, so a
  Mac stops re-running `claude --version` on every poll.

`batch-shape.sh` §14 greps both publishers for `\|`, `\+` and `\?` outside an
ERE, since Linux CI cannot run BSD sed and this is the only place the class is
visible. It fails on the pre-sprint script, naming line 83.

### Proved on kimac, not only in CI

Piped through `ssh kimac 'bash -s'` (bash 3.2.57, Darwin), nothing written to
the host: `/bin/bash -n` passes on both publishers, and the helpers return the
same epochs as GNU on kai for `…Z`, `….123456+00:00`, `-07:00` and `+0530`
stamps, `mtime` matches `date -j`, and `jstr` reads the event, an escaped
quote, and the top-level `cwd` over a nested one.

## Version

`publisher/VERSION` 2.3.0 → **2.4.0**: a new file in the bundle and the first
release that runs on macOS, which a k-homelab pin must reach for kimac to
publish. The bundle version recorded for the pin bump is the one published from
`main` at ship time.

## Acceptance

WI 3170's harness, extended with three controls, run in the foreground
2026-09-23 ~22:53 PDT. It ran against the **store artifact**, not the checkout.
`d4e01cf` was published from the branch as `2.4.0-d4e01cf` with `--no-latest`,
and the harness fetched `claude-pub.sh` from the store into a temp dir and
checked it against the bundle's `SHA256SUMS` before running it. The kimac run
was probed **from kai**, piped through `ssh kimac 'bash -s'`. It left nothing on
the host: the temp dir and state dir were removed on exit.

The three controls:
- `kdash-pub check` runs first, so an empty scan cannot be a failed
  connection.
- The installed 2.3.0 runs as the "before".
- The scan runs again after `SessionEnd(clear)`.

| | kimac (Darwin 27, bash 3.2.57) | kai (Linux, bash 5.2) |
|---|---|---|
| `kdash-pub check` | PING answered, auth from `/etc/khomelab/secrets.env` | same |
| installed `2.3.0-69cad0d`, SessionStart `t0` | **nothing published** (the bug) | published, as it should |
| `2.4.0-d4e01cf`, SessionStart `t1` | `claude:session:kimac:t1`, host/project/cwd/status/ts/started_ts all set | `claude:session:kai:t1-043h`, same fields |
| after SessionEnd `reason: clear` | gone | gone |

On kai, the 2.3.0 control's fields (`host=kai project=x cwd=/tmp/x
status=working`) match 2.4.0's, so the Linux feed is unchanged. That control
key was then cleared with the installed script's own `SessionEnd(clear)`. A
final scan of both hosts' `t*` keys came back empty.

The version k-homelab pins is the one published from `main` after the squash
merge. Its payload is byte-identical to `2.4.0-d4e01cf`, but its sha is the
merge commit's.

## Repaired in passing

- **`jstr` returned the last occurrence of a field, not the first** (above):
  a nested field in `tool_input` could shadow the top-level `cwd`/`project` on
  every platform. Fixed by the same change; proven by `batch-shape.sh` §13.

## Deployed

2026-09-23 ~22:57 PDT, from merged `main` at `c44430a` (PR #50), both
`.sprint-deploy` steps run in the foreground:

- **`deploy-panels`** — `just publish` answered `nothing to publish: kdeskdash
  0.27.0-77925f2 already in the store`: no panel code changed this sprint.
  `just versions` shows rpidash2 and rpidash3 both on `kdeskdash
  0.27.0-77925f2`, so neither was installed or restarted.
- **`just publish-publisher`** — published **`kdeskdash-publisher
  2.4.0-c44430a`**, `latest -> 2.4.0-c44430a`. Its `SHA256SUMS` matches the
  branch build `2.4.0-d4e01cf` on every file except the generated `VERSION`.
  **This is the version k-homelab pins** (korg:3143).

Post-ship acceptance was re-run against `2.4.0-c44430a`, fetched from the store
and sha-verified, probed from kai:

- **kimac:** `claude:session:kimac:t1` was written with all six fields, and
  `SessionEnd(clear)` removed it. The installed 2.3.0 control still publishes
  nothing.
- **kai:** `claude:session:kai:t1-043h` was written with the same fields, and
  cleared.

The kai control key was cleared too, and a final `t*` scan on both hosts came
back empty.

Nothing is installed on any feed host by this sprint. Installing is k-homelab's
pin bump: `claude-hooks` on kai, kubs0 and kimac, and `copilot-hooks` declared
on kimac with `ghcp-hooks.darwin.json` (korg:3143).
