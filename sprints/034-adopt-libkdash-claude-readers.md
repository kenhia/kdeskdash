# 034 — Adopt libkdash's claude readers, retire the local pair

korg: proposal 2218 / work item 1783. Slice 4 of the backlog-drain program
(korg:2233), run as an overseen karc leg.

**Goal:** kdeskdash stops carrying its own copy of the `claude:*` feed logic
and reads it through libkdash, the shared consumer library kdashdata owns.
`src/claude_feed.c` (356 lines) and `src/claude_redis.c` (242) are deleted, not
shadowed.

## Premise check

The item was written 2026-09-02 and said "once libkdash ships `claude:*`
readers (kdashdata 006)". Checked before touching anything, because this
program's operating assumption is that an undrained queue decays into items
that are already true:

| claim | verdict |
|---|---|
| `claude_feed.c` 356 lines, `claude_redis.c` 242 | holds, exactly |
| libkdash ships the readers | holds — kdashdata sprint 006 (`f95a233`, 09-03); sprint 009 (`bb29b69`) added the counted-reader −1 rule |
| "check whether the derived display status and attention-first sort landed — if they did, this is a deletion" | **they did** (`kdash_claude_display`, `kdash_claude_sessions_refresh`), so this is a deletion, not a move of the I/O half |
| "kstudiodash is the consumer that needs the readers now" | drifted, favourably: kstudiodash *already* consumes libkdash as a `lib/kdashdata` submodule |

That last one decided the shape of the sprint. There was a worked pattern to
copy rather than a mechanism to invent, so nothing here needed a decision in
another repo and no part of it was a Branch-B park.

## What shipped

**libkdash as a submodule.** `lib/kdashdata` pinned at `bb29b69`, wired with
`add_subdirectory` and linked as `kdash`, following kstudiodash exactly.

Three things in that wiring are deliberate and easy to get wrong:

- **`KDASH_HIREDIS_STATIC` is not set.** kstudiodash forces it ON because the
  machine it deploys to has no libhiredis at all. Both Pis already run a
  kdeskdash that links `libhiredis.so`, so forcing static would have changed
  this project's deploy story as a side effect of a refactor. kdeskdash creates
  `hiredis::hiredis` first and kdashdata's own lookup is guarded by
  `if(NOT TARGET ...)`, so exactly one hiredis is linked. (kdashdata then prints
  `hiredis: using CMake config`, which is its else-branch message and not what
  happened — cosmetic, in the submodule, left alone.)
- **Two vendored cJSONs, one in the binary.** Both trees carry cJSON 1.7.18,
  byte-identical (diffed). kdeskdash compiles its copy into the executable;
  `kdash_core` archives its own. The linker satisfies `kdash_core`'s references
  from the executable's objects and never pulls the archived member, so there
  is no duplicate-symbol problem and no version skew to reason about.
- `KDASH_BUILD_EXAMPLES` / `KDASH_BUILD_TESTS` default OFF when kdashdata is
  not the top-level project, so `kdash_dump` and its ctest registrations stay
  out of this build without being named.

**The pair is gone.** `src/claude_feed.{c,h}` and `src/claude_redis.{c,h}`
deleted, along with `tests/test_claude_feed.c`. Three stale comments naming
`claude_redis.c` as a sibling implementation unit (`redis.h`,
`redis_internal.h`, `kvscf_redis.c`) were corrected rather than left pointing at
a file that no longer exists.

**What did *not* move, and why that is not shadowing.** libkdash hands back the
enum's own lowercase name (`"blocked"`) and leaves an absent `project` as `""`,
because CD-10 keeps rendering on the panel. The words on this 1920×440 screen —
`BLOCKED ON YOU`, the `?` placeholder, the compact age string, the reset clock
format, the 80% warn threshold — are this project's and nobody else's. They
live in a new pure core, `src/modes/claude_view.c`, tested by
`tests/test_claude_view.c`. It is named for what it is: rendering, not a second
reader. Nothing in it parses a payload, derives a state, or opens a socket.

**The mode owns its handle.** `claude_mode_create()` now takes the endpoint and
builds its own `kdash_conn_t`; `main.c`'s
`if (modeset_enabled(&modes, "claude")) claude_redis_init(...)` block is gone.
The feed now exists exactly when the mode does, so the "only dial an endpoint a
registered mode uses" rule is structural instead of a second roster to keep in
step with `modeset.c`. `main.c` keeps one pointer purely so teardown can close
the handle. Verified live: rpidash3 does not register `claude` at all, and
therefore never constructs the handle.

## Three behaviour traps, all found by reading both sides

**1. The endpoint pin survives — checked on the wire, not asserted.**
`kdash_conn_opts_t.host` is an explicit override: "when set, khlenv is never
consulted." `config.c` always supplies a host, so this handle never resolves an
endpoint at runtime and both boards' explicit `rpi53:6379` pin from sprint 031
keeps doing exactly what it did. Confirmed on rpidash2 by looking at the
process's actual sockets: `192.168.1.144 -> 192.168.1.213:6379`, and
`rpi53 = 192.168.1.213`. No khlenv, no 6380.

**2. `auth` inverts, and silently.** libkdash reads `$REDISCLI_AUTH` when `auth`
is NULL; the retired client sent no AUTH at all. A bare `REDISCLI_AUTH` is a
documented option in `kdeskdash.env.example`, so NULL had to become `""` —
libkdash's spelling of "no AUTH" — or a board could have started authenticating
with the control Redis's password on a refactor that was supposed to change
nothing. This is the failure that would have looked like a content error and
been blamed on the port.

**3. A counted reader returns −1 or a complete list.** `kdash_claude_sessions()`
is one (kdashdata sprint 009), so a negative return means the read did not
complete and *neither* `out` nor `*skipped` carries information. Rendering that
as zero rows would say "nobody has Claude open" — a confident wrong answer, and
exactly the partial-list failure kdashdata just removed. `scan_sessions()` drops
`have_sessions` on a negative return and keeps the previous list; `poll()` turns
that into the existing quiet banner over the last-rendered panel.

## The one cadence decision

The retired client had a hand-rolled *incremental* SCAN (`discover_step`
advanced one batch per call, every 5 s) to keep the UI thread free.
`kdash_claude_sessions()` does the whole SCAN + HGETALL in one call, so the
reason for the split is gone — but folding it into the 2 s render tick would
have raised this panel's SCAN rate 2.5× against a Redis three dashboards read.
That is a load change nobody asked for, arriving as a side effect of a
refactor, so the two cadences were kept: sessions scan at `CLAUDE_SCAN_MS`
(5 s, today's rate), and rows re-render from the cached array every
`CLAUDE_POLL_MS` (2 s) so the age column still ticks at today's rate.
`kdash_claude_sessions_refresh()` runs on every render rather than once per
scan, because `disp` is a function of `now` — a session has to be able to age
into IDLE and STALE between scans.

## Tests

`tests/test_claude_view.c` replaces `test_claude_feed.c`. 21/21 green, same
count: the feed's own contract is tested in kdashdata now.

Every new assertion was watched failing before being trusted:

| break | caught by |
|---|---|
| widened `BLOCKED ON YOU` past the fixed status column | label test **and** the width-vs-`AWAITING INPUT` assertion |
| dropped the `""`/NULL → `?` placeholder | placeholder test — and the NULL case segfaults, which is why the guard is there |
| claimed `working` does *not* degrade at the idle threshold | the ladder-contract test (proving it is not vacuous) |

That last one is deliberate and worth defending: this panel renders a
derivation it no longer owns, and its layout is built around one specific
behaviour — `blocked` and `awaiting` stay prominent through the idle band while
`working` degrades. libkdash tests its own ladder; this says out loud which part
of it kdeskdash is standing on, so a change there fails here rather than quietly
changing what the panel shows. It asserts, it does not re-derive.

**Not covered, and honestly so:** the −1 path has no test. Triggering a
mid-scan drop means killing a Redis three dashboards read, and `kdash_feed.c`
is the I/O shell CD-10 keeps out of `just check`. This is the gap kdashdata
WI 2246 was filed for; kdeskdash inherits it rather than adding to it.

## Verified on the panel, not in the build

Both boards, pushed with `just push-dev` (`0.27.0-5073ab9-dirty`) and restarted
under systemd. Probes were run from **kai**, which is the host that
cross-compiles and does the pushing.

- **rpidash2** — Claude mode screenshot via `scripts/kddss`: five session rows
  in attention-first order (two amber `AWAITING INPUT` above three green
  `WORKING`), host / project / session-name / model columns populated, ages
  (`2m`, `15m`, `39s`, `1m`), `+9 more` overflow, header count "5 working • 2
  waiting on you", and all three usage gauges — 5 HR 38%, 7 DAY 14%, and the
  model-scoped `FABLE` 9% — with `resets 01:50` / `resets Thu 05:00` and
  `as of 1m ago`. That exercises the whole port end to end on live data: the
  library's parse, sort and ladder; this panel's labels, placeholder, age and
  reset formatting; and the scoped-gauge path. AUTH is proved by the rows
  existing at all.
- **rpidash3** — registers no `claude` mode
  (`KDESKDASH_MODES=fun:icons,palette;ops:launcher,foreground,clock,dev,calc`),
  so the mode switch was correctly refused and the panel stayed on Launcher.
  The new binary runs clean there: 7 modes, no errors in the journal, screenshot
  works. Its value here is the negative one — it is the live check that a panel
  without the mode never constructs the handle.

## Follow-ups

- The store deploy is the ship's job; these were dev pushes so the boards
  report `-dirty` rather than implying a published build.
- rpidash2 was left on Claude mode. Its previous mode was not captured before
  the switch, so it was not restored; rpidash3's key was set back to `launcher`
  to match what it displays.
