# Sprint 041 — Declared deploy with a no-op predicate; the feed stops latching `working`

**Proposal:** korg:2980 (slice of program korg:2981, "Low-hanging fruit —
experiment 1").
**Covers:** WI 847 (link the ABS notes), WI 2801 (declare both artifacts with a
no-op predicate), WI 2719 (a re-attached session latches `working`), WI 2809
(a headless `claude -p` exit leaves the row at `working`).
**Branch:** `041-declared-deploy-and-honest-session-states`, off `c7d230e`.

Overseen sprint: run as karc leg `kdeskdash-de6491` on kai, reviewed on the
proposal thread, shipped on the overseer's green light.

## Goal

Four unrelated small items, three of which turn out to share one shape: a
recipe, a publisher and a panel each reporting something that was true once and
is no longer checked.

## Premise check

All four held.

- **847** — `README_ABS.md` was in `.scratch/` and untracked, `docs/hardware.md`
  had the ABS section and no link to it, the "Still to verify" checklist was
  unticked. Ken's answer (2026-09-11) was on the item.
- **2801** — `.sprint-deploy` named only `deploy-panels`.
- **2719** — `claude-pub.sh`'s SessionStart branch wrote `status working`,
  `ts NOW` and `started_ts NOW` unconditionally.
- **2809** — `SessionEnd` used `send_sync`; `Stop` went through the
  backgrounded `send`.

**And 2801's premise was live, not just true.** At sprint start `HEAD` was
`c7d230e`, a docs-only commit, while the last commit touching the panel payload
was `245ad8a` — which *is* the store's `latest` and what both boards run. The
old recipe would have republished a byte-identical binary under a new version
and restarted both Pis for it.

## WI 2809 — what the measurement actually found

The overseer's ruling (2026-09-17) said to measure first, and named the likely
answer: *"SessionEnd does not fire at all when a print-mode `claude -p` exits"*.

**It fires.** An event-logging hook run over three `claude -p` shapes — plain,
under a transient `systemd-run --user` unit, and with a tool call — recorded
every lifecycle event in all three, `SessionEnd` included, with `reason=other`,
13–19 ms after `Stop`:

```
21:00:19.323 event=SessionStart
21:00:20.849 event=UserPromptSubmit
21:00:22.063 event=Stop
21:00:22.076 event=SessionEnd      reason=other
```

Nor is the hook killed: a `SessionEnd` hook that sleeps **6 s** runs to
completion, and driving the real `SessionEnd` branch by hand against a seeded
key deletes it correctly. So neither "the event is missing" nor "the hook is
killed" is the fault, and **branch 3 of the ruling does not apply** — there is
no Claude Code gap to record and no new decision for Ken about karc publishing
the terminal state.

### The actual mechanism: unordered fire-and-forget writers racing the DEL

`send()` publishes from a **disowned background child**, so a session's events
are four *concurrent* publishers against one key with no ordering between them.
`SessionEnd`'s DEL is synchronous and therefore lands — and then a straggler
from an earlier event lands on top of it and re-creates the row.

The surviving rows say so themselves. Three sessions run with the real
publisher, read 20 s after exit, all three left at `working`:

```
status=working  ts=21:01:33  started_ts=21:01:31  title=OK
```

`started_ts` is written **only** by SessionStart and `title` only by the
enrichment block that `SessionEnd` never reaches (it `exit 0`s inside the
case). A row carrying both, after a DEL that demonstrably ran, is a row that
was re-created by writes queued before it. The instrumented run makes the other
half visible: `Stop` fired, and produced **no publish attempt at all** — its
disowned child was torn down with the process before it could reach the network.

So the two reported symptoms are one cause. `Stop`'s `awaiting` is lost, and
the DEL is overtaken by whichever earlier write happens to land last.

### The fix, and why it is only one line of behaviour

`Stop` now publishes through `send_sync`. That removes the only straggler close
enough in time to overtake the DEL (13 ms; SessionStart and UserPromptSubmit are
seconds earlier and complete in ~10 ms), and it makes `awaiting` — the honest
state for a turn that ended — actually arrive. It is the ruling's branch 2, and
the cost is milliseconds paid at a turn end where nothing is waiting on it.

Measured, same harness, 6 sessions across both `async` settings of the Stop
hook declaration:

| | rows left behind |
|---|---|
| before | **3 of 3** left at `working` |
| after | **0 of 6** — all deleted cleanly |

**No hook-declaration change is needed, and therefore no cross-repo change.**
It works with `Stop` still declared `"async": true`, so k-homelab's
`claude-hooks` recipe — which owns the live settings on kai and kubs0 — is
untouched.

The residual race (a SessionStart or UserPromptSubmit write landing seconds
late) is not closed. It is not reachable in the measurements and closing it
would mean making the interactive path synchronous, against the publisher's
founding rule that a dead Redis must never slow a Claude session down.

## WI 2719 — a re-attach is not a cold start

The desktop app re-opening a finished session spawns a **fresh** CLI carrying
the old uuid and no `--resume`, so `source` reads `"startup"` exactly as a new
session does — and the emit site is skipped on a real `--resume`, so `source`
can never carry the distinction. The transcript can: one that already has
assistant turns belongs to a session that has been used.

Taking the WI's second option (narrow, no contract change): a re-attach
publishes **`awaiting`**, a status the feed already publishes at `Stop`, so no
consumer learns a new word. A cold start still publishes `working`.

`started_ts` survives the re-attach too — from the local `.start` file, or, when
the two-day `STATE_DIR` sweep has removed it (precisely the age of a session
worth re-attaching), from the transcript's first timestamp. Overwriting it was
the second-order damage the WI names: session age wrong, and `SessionEnd`'s
`dur_s` measured from the re-attach, pushing a bogus short entry onto
`claude:recent`.

## WI 2801 — one rule, two artifacts, declared unconditionally

Ken's decision (option 1, 2026-09-17): the recipes own change detection, and
both lines get declared.

- **`scripts/version.sh` is now payload-scoped.** The panel version is the last
  commit touching `src/ lib/ lv_conf.h CMakeLists.txt cmake/` + the font, the
  unit, the env example, `scripts/deploy.sh` and `VERSION` — not `HEAD`. Same
  derivation the publisher bundle has always used, same rule klaude-top adopted
  (its WI 2782).
- **`scripts/store-has.sh` is the predicate**, with three outcomes: present (0),
  absent (1), **could-not-ask (2)**. `ssh … test -d` is the trap — an absent
  version, a downed store, a missing host key and a refused login all exit
  non-zero, and calling any of them "absent" republishes over a store nobody
  can see. The remote answers with a word it prints itself. Both publish
  recipes treat 2 as a refusal.
- **`scripts/version-publisher.sh`** now owns the bundle's version derivation,
  so the payload list has one home rather than being restated in the justfile.
- **`.sprint-deploy` declares both** `deploy-panels` and
  `recipe: publish-publisher`. Neither needs a condition: each asks about its
  own artifact and exits 0 saying `nothing to publish` when there is none.

Exit 0 on the no-op is the point. `kpkg`'s immutability guard already gave the
correct *answer*; sprint-ship Phase 7 reads a non-zero step as a loud deploy
failure, so the answer was in the wrong *shape* and every unrelated sprint
would have ended on a false alarm.

`just published [version]` / `just published-publisher [version]` expose the
predicate; the exit code is the answer.

### The input-set gap, recorded rather than closed

The payload names the files that ship, not the machinery that ships them, so
`publish.sh`, `version.sh` and the `justfile` are excluded (Ken's decision).
Changing **only** the build flags in publish.sh's own `cmake` invocation would
alter the binary without moving the version. The remedy is a `VERSION` bump,
and `docs/deploying.md` says so. The toolchain and its compiler options are in
`cmake/`, which *is* payload, so the gap is that one line.

## WI 847 — the ABS notes, published with the answer on them

Ken answered the blocking question on 2026-09-11: the **100.545 % was a uniform
scale of the model in the slicer** — the per-object scale box, the right
mechanism — not the filament Shrinkage field, and ABS is still not recommended.
So the notes' guidance stands as written, including "leave the filament
Shrinkage field at 100".

`.scratch/README_ABS.md` → `stl/README_ABS.md`, tracked, and linked from the
ABS section of `docs/hardware.md`. The checklist item that asked *which field*
is ticked with the answer; the one that asked whether 100.545 % lands the body
at 277 stays **open**, and says explicitly that no dimensional result was ever
recorded. No measurement was invented, and the link is worded so it cannot read
as "ABS is ready now" — the blind overhang is untouched.

`docs/assembly.md` gets no pointer, as the WI expected: it is a build-order
page and shrinkage is a printing concern.

## Repaired in passing

- **`set -e` would have swallowed the predicate.** Both publish scripts run
  under `set -euo pipefail`, where a bare `scripts/store-has.sh …` followed by
  `case $?` dies at the call — the three outcomes would never have been read.
  Fixed to `|| rc=$?`, which is a tested context. Caught by running the recipe,
  not by reading it.
- **An unset `KDESKDASH_STORE_HOST` answered "absent".** `${VAR:?}` exits 1,
  which is exactly the code this script defines as "not in the store" — so a
  missing config line would have triggered a republish. It is a could-not-ask
  and now exits 2. Verified alongside an unresolvable host and a host with no
  `kpkg`.
- **The publisher payload list was briefly in two places** (the new justfile
  recipe and `publish-publisher.sh`). Extracted to
  `scripts/version-publisher.sh` before it could drift.

## Tests

`publisher/tests/batch-shape.sh` gains six assertions; the four that are
regression tests were confirmed to **fail** against the pre-fix script and pass
against the fixed one.

- a re-attached session publishes `awaiting`, not `working`
- a re-attached session keeps its original `started_ts`
- a swept `.start` falls back to the transcript's first stamp
- a genuinely new session still publishes `working`
- `Stop`'s batch is delivered before the hook returns — **no poll and no sleep,
  which is the assertion**
- `Stop` still publishes `awaiting`

Gate: `just check` green, 24/24.

## Acceptance

Proven in-session:

| check | result |
|---|---|
| `just check` | green, 24/24 |
| new assertions vs pre-fix script | 4 fail / vs fixed: all pass |
| headless exit leaves no row | 0 of 6, against 3 of 3 before |
| `SessionEnd` fires in print mode | 3 of 3 shapes, `reason=other` |
| `store-has.sh` present / absent / never-published | 0 / 1 / 1 |
| `store-has.sh` unreachable / no kpkg / unset host | 2 / 2 / 2 |
| `just published` on an unchanged payload | `present: kdeskdash 0.27.0-245ad8a` |

Left to the ship's Phase 7, which is the first end-to-end run of the
declaration and is a **trigger, not a soak**: `deploy-panels` no-ops its
publish (this sprint touches no `src/`/`lib/`) while `recipe:
publish-publisher` publishes the bundle, because the publisher did change.
No soak work items were created.

**`VERSION` is deliberately not bumped** — this sprint changes no panel
payload, and bumping it would force exactly the republish of an identical
binary that WI 2801 exists to prevent. `publisher/VERSION` goes 2.2.0 → 2.3.0:
what a session row says about itself changed, and consumers read that.

## Probes and hygiene

Every probe ran **on kai**, which is the host that publishes and the host the
recipes run from. The measurement sessions used `--setting-sources ''` so the
live publisher hooks could not be disturbed; the three runs that deliberately
used the real publisher left rows in the live feed and were deleted afterwards
(verified absent). Scratch under `.scratch/2809/`, gitignored.
