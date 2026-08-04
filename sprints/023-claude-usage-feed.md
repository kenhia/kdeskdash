# 023 — Claude usage: session-free feed, Fable gauge, per-gauge staleness

WIs #951–#956 · proposal korg:957 · publisher + claude mode + docs

## Goal

The USAGE gauges had exactly one writer — the statusline hook — so they froze
the moment the last session ended and kept showing the last number they saw,
indefinitely and without saying so. This sprint gives `claude:limits`
session-free writers (the desktop app's own usage file on cleo, the OAuth
usage endpoint on kai), adds the model-scoped (Fable) weekly window as a third
gauge, and makes staleness per-gauge and visible. The publisher half was built
and validated on cleo the day before (2026-08-03) but existed only in the
deployed copy — the next deploy would have silently reverted it, which is why
this didn't sit in the queue.

## What shipped

- **[publisher/claude-pub.sh](../publisher/claude-pub.sh)** — the `poll` mode
  landed (file + oauth sources, observation-time stamps, read-back no-clobber
  guard), then extended: the oauth source walks `limits[]` and publishes the
  `scoped_*` field set under its **own** stamp and guard, and every writer
  publishes its cadence as `expected_refresh_s` / `scoped_expected_refresh_s`.
- **[publisher/poll-hidden.vbs](../publisher/poll-hidden.vbs)** — the Windows
  shim that keeps the 5-minute task from flashing a console window.
- **[publisher/kdeskdash-claude-poll.service](../publisher/kdeskdash-claude-poll.service)
  / [.timer](../publisher/kdeskdash-claude-poll.timer)** — the systemd user
  pair, installed and running on kai (recorded as k-homelab WI #963). cleo's
  deployed script was re-synced **from the repo** and verified byte-identical.
- **`claude` mode** — USAGE is a 2-over-1 triangle (arcs 170 → 122): 5 HR +
  7 DAY on top, the scoped weekly centred below, labelled from `scoped_model`
  verbatim; "as of" moved onto the header line, right-justified; each gauge
  greys **only its percentage** when its own stamp exceeds its writer's
  cadence + 60 s. No scoped set → the bottom row hides and two gauges remain a
  first-class layout.
- **[src/claude_feed.c](../src/claude_feed.c)** — scoped parsing +
  `cf_limits_scoped_stale()` in the pure core, tests first
  ([tests/test_claude_feed.c](../tests/test_claude_feed.c)).
- **Docs** — publisher README: source decision table, the field-by-field
  `claude:limits` contract, both install paths, and a plain terms section; the
  generalized pattern in
  [docs/solutions/best-practices/independent-writers-need-independent-stamps.md](../docs/solutions/best-practices/independent-writers-need-independent-stamps.md).

## Decisions

**The scoped set gets its own read-back guard, not just its own stamp.** The
handoff called out the trap (a fresher file write can't supply `scoped_*` and
must not freeze it) and prescribed `scoped_updated_at`; implementing it showed
the same reasoning applies to the *write* side: under the single headline
guard, a fresher cleo file write would make kai's poll exit before publishing
the scoped set at all, skipping scoped updates for a cycle. So `poll` guards
the headline set on `updated_at` and the scoped set on `scoped_updated_at`,
independently. Rule 2 of the pattern applied per stamp.

**Staleness policy ships with the writer, thresholds live nowhere.** Writers
publish `expected_refresh_s` (statusline 60, poll 300); the panel greys past
stamp + cadence + `CF_LIMITS_GRACE_S` and knows nothing about sources. A
pre-cadence hash falls back to the legacy fixed hour; a scoped set with data
but no stamp renders stale from the start rather than borrowing headline
freshness.

**Only the percentage greys.** The arc and reset caption keep rendering the
last-known data; the number stops claiming it is live. A frozen bright number
is indistinguishable from a fresh one — that was the whole bug.

**Caption from `scoped_model`, uppercased, never matched.** The API exposes a
display string with a null model id; "Fable" is data, not schema. If
`scoped_count` ever exceeds 1 the flat fields become indexed — published now
so the door is already open.

**"resets ---" on the panel was the design working, not a bug.** Investigated
at sprint start: the key had been deleted and repopulated during the cleo
validation, and the file source deliberately never writes reset stamps it
doesn't have. kai's first oauth poll restored them permanently.

**The 2-over-1 triangle over three-across.** Ken's sketch, confirmed by the
arithmetic: two ~193 px top columns still clear the ~176 px worst-case reset
line from the 2026-07-03 calibration, where three-across columns (~120 px)
would not. The height budget (392 px content) caps the arcs at ~123 px with
both text lines kept; the percentage font drops 36 → 28 to match.

**`scoped_active` is parsed but not yet rendered.** The binding window today
is `weekly_all`, which has no gauge marker of its own; emphasis treatment is
deferred until the flag would actually distinguish something on screen.

## Verification

- **Publisher, offline:** a ~60-line fake RESP server + mocked `curl`/
  `claude`/credentials/usage-file ran the three cases that matter — full
  oauth write; *fresher* file write (headline moves, scoped set and reset
  stamps untouched); *staler* file write (refused entirely). The first run
  also caught bad test data: a file sample 30 s older than the oauth
  observation was correctly refused, which is the guard doing its job.
- **On device (rpidash2), pixel-verified:** stamps aged directly on the live
  key; sampling the glyph cores in `kddss` screenshots confirmed
  scoped-only-stale greys exactly the FABLE percentage (ink 0xe9edf6 →
  muted), all-stale greys all three plus the amber header readout, and one
  kai poll heals everything — the self-healing loop from the acceptance
  criteria. Deleting the scoped fields produced the clean two-gauge layout;
  the next poll restored the third gauge.
- **Live feed:** kai timer active (`source=oauth` writes with the full scoped
  set); cleo task re-synced and green; `just check` — 16/16 tests.

## Follow-ups

- **Multiple scoped windows.** `scoped_count` is published; if the API ever
  returns per-model *and* per-surface entries, the flat fields become indexed
  and the panel grows a second scoped gauge or a rotation.
- **`scoped_active` emphasis** once the binding constraint is representable
  on screen (e.g. a marker on whichever gauge `is_active` names).
- **Countdown honesty on file-only fleets.** A fork with no oauth host ages
  its reset captions while percentages stay live; the panel could suppress a
  countdown older than its stamp. Moot for this fleet while kai polls.
