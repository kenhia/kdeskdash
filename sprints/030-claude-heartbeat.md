# Sprint 030 — keep a working session from greying to IDLE

korg:1361 (proposal) / #1360 (work item). Slice 1 of 2, paired with k-homelab
#1362 — script first, matcher second, and unlike the usual pairing the order
does not matter (see below). Publisher-only: no panel binary changes, so
nothing is deployed to the boards this sprint.

## Goal

Ken, from the desk panel: sessions that work for a long time go grey while the
agent is still cranking. He guessed "exactly 15 minutes" and was right —
`CF_IDLE_S` is `15 * 60` in `src/claude_feed.h`.

The cause is on the publishing side, not the panel's. `cf_display_status()`
only decays `working`; `blocked` and `awaiting` deliberately hold their label
until STALE at 40m. So the ladder is doing exactly what it was designed to do,
and the defect is that nothing refreshes `ts` during a working stretch:
`claude-pub.sh` wrote it on SessionStart / UserPromptSubmit / Stop / SessionEnd
and on PreToolUse/PostToolUse *scoped to `AskUserQuestion`*. Between a prompt
and a Stop, a forty-minute turn published nothing at all.

On a glance device a wrong liveness signal is the worst defect available — it
trains you to stop trusting the zone.

## The shape, and why this one

Tool hooks on every tool, taking a fast path that refreshes `ts` and exits.

**`ts` only, never `status`.** This is the whole safety argument. A backgrounded
Bash call or a subagent can fire tool hooks while the main agent sits on an
`AskUserQuestion`, so a keepalive that wrote `status working` would silently
downgrade a `BLOCKED ON YOU` row — the highest-attention state the panel has —
to WORKING. Writing one field means the keepalive can make a row fresher and
can never change what it claims. The dashboard needs no change to accept it:
`cf_session_from_fields` wants `status` and `ts` in the *hash*, not in any
single write, and ignores unknown fields.

The alternative considered and rejected was a separate `hb` field with the
panel taking `max(ts, hb)` for the ladder and keeping `ts` for the age readout.
It preserves the age readout's exact meaning, but it costs a parser change, a
ladder change, and a deploy to both boards — one of which (rpidash3) is
routinely off-network, so it would have been half-rolled indefinitely. Not
worth it for a semantic nuance.

**Throttled, and the fast path is ordered first.** `KDD_HEARTBEAT_MIN_S=120`
with a per-session `<sid>.hb` stamp, the same shape as the statusline's
`limits.stamp`: seven writes cover a 15-minute window, so one dropped heartbeat
can never grey a row. The branch sits above `cwd`/`project`/`transcript_path`
in `hook_mode` because it is now the hottest path in the script — once per tool
call in every session on the box — and pays for only the three fields it needs.

## Accepted, not overlooked

- **A single long tool call still greys.** No hook fires *during* a tool, so a
  25-minute build under `Bash` crosses `CF_IDLE_S` regardless. Closing it needs
  a background timer; Ken called the gap out when he raised this and accepted
  it ("isn't a perfect solution, but strikes the right balance"). Deliberately
  not a first step toward a daemon.
- **A per-tool-call fork.** Unmatched hooks mean the script runs on every tool
  call on every publishing machine. Throttled out that is a bash start plus
  three `sed`s, worst on cleo where Git Bash process creation is slow.
- **`ts` changes meaning**, from "last lifecycle event" to "last sign of life",
  and the row's age readout follows. Better for a glance device, but a real
  change; it is in `publisher/README.md`.

## No ordering constraint — stated because you will expect one

The `blocked` rollout DID have one (`cf_session_from_fields` rejects an unknown
`status`, so rows *vanish* against an old binary) and `publisher/README.md`
documents it. This has none, in either direction:

- Matcher first, old script: every tool call invokes `claude-pub.sh`, which
  re-checks `tool_name`, fails, and exits. Wasted forks, no wrong data.
- Script first, old matcher: the keepalive branch is never reached. Dead code.

The keepalive adds no new field and no new status value, so old panel binaries
read it correctly too.

## What shipped

- `publisher/claude-pub.sh` — `heartbeat()`, `KDD_HEARTBEAT_MIN_S`, the fast
  path at the top of `hook_mode`, and `<sid>.hb` added to the SessionEnd
  cleanup. The old `AskUserQuestion` re-check in the case branch is gone: the
  fast path routes everything else away before it, so it was unreachable.
- `publisher/settings-fragment.json` — PreToolUse/PostToolUse matcher `*`, plus
  `//managed` and `//tooluse` notes. This file is now the reference for
  **unmanaged** hosts (cleo) only.
- `publisher/README.md` — "Keeping a working session alive" and "Where the hook
  registration lives"; the Blocked-on-you section's matcher claim corrected.

## Verification

`.scratch/hb-test.sh` (scratch, not in `just check`) drives the hook against a
fake RESP sink on a scratch `HOME` and asserts 19 properties: the heartbeat
sends `HSET ts` + `EXPIRE` and **no** `status` or `project`; the throttle
suppresses a second tool call inside the window; a backdated stamp fires again;
`AskUserQuestion` still publishes the full `blocked`/`working` record and is
**not** suppressed by the heartbeat stamp; SessionEnd clears the stamp and
still sends `DEL`. 19/19. `just check` green, 17/17 — no C changed, so that is
a no-regression check rather than evidence.

Not exercised here: a real >15-minute session against the live feed, and the
negative case on a real panel (a blocked row staying BLOCKED while background
tool calls fire). Both need the k-homelab half applied on kai/kubs0 first.

## Follow-ups

- **k-homelab #1362** — the `settings_merge.py` matcher, the
  `kdeskdash_publisher_version` pin, and applying `claude-hooks` to kai and
  kubs0. Nothing reaches the managed hosts until it lands.
- **cleo** is unmanaged: hand-install per `publisher/README.md`.
- Deploy litter (`deploy-*.png` left in the tree by a deploy) refuses
  `just publish-publisher`, which checks `git status --porcelain` — untracked
  files included. Cleared by hand this sprint; `chore-deploy-cache-litter` on
  origin looks aimed at the recurrence.

## Deployed 2026-08-16

Shipped as PR #37, squash `4984a84`.

- **Publisher bundle**: `kdeskdash-publisher 1.0.0-4984a84` published from main,
  `latest` moved to it. Rollback target: `1.0.0-0d6a98d`.
- **Panels: deliberately not published or deployed.** No C changed, so the
  binary is byte-identical to the `0.27.0-0d6a98d` both boards already run, and
  `just versions` stays uniform without touching them. Sprint 029 did publish a
  same-bytes stamp, but it was proving the publish path itself; doing it here
  would add a store version that is not a distinct build, which makes the
  rollback history less useful rather than more. The panel's clock is allowed
  to stand still when the panel does.

**Verified** by fetching the bundle back over the store URL rather than trusting
the publish output: `latest` (a pointer *file*, not a directory — it holds the
version string) resolves to `1.0.0-4984a84`, the in-band `VERSION` agrees, all
four payload files pass `SHA256SUMS`, the published `claude-pub.sh` carries
`KDD_HEARTBEAT_MIN_S`, and it is byte-identical to `publisher/claude-pub.sh` on
merged main.

**Live nowhere yet, by design.** Nothing consumes the new bundle until
k-homelab #1362 (proposal korg:1368) bumps `kdeskdash_publisher_version` and
re-applies `claude-hooks` on kai and kubs0. cleo's hand install is still
pending. Until then the behaviour on the panel is unchanged — a still-greying
row is expected, not a regression.
