# Sprint 047 — claude-pub stops copying 7-day values into the 5-hour fields

**Proposal:** korg:4634 (slice 7 of program korg:4619, "kpidash row 2: Claude
limits card and korg + klams card").
**Covers:** WI 4623.
**Branch:** `047-claude-pub-five-hour-none`, off `0a6bc04`.

This is an overseen sprint. It ran as karc leg `kdeskdash-835b14` on kai, and the
overseer's review happens on the proposal thread. The brief is the proposal's
notes, its comment 4674 carrying kdashdata CD-29 forward, and WI 4623's comments
4662 (cause) and 4665 (Ken's display ruling).

## Premise check

- **The fault holds, in both parse paths.** `from_oauth()` tested
  `[ "$fhb" != "$body" ]` *after* `fhb="${fhb%%\}*}"`. With `"five_hour":null`
  the first expansion matches nothing, the trim cuts the whole body at its first
  `}` (the end of `seven_day`), and the guard can never fire. `jnum`/`jstr` then
  read seven-day's utilization and reset as five-hour ones. `statusline_mode()`
  had the same shape with no guard at all.
- **The API still says null.** A body read on kai at 2026-11-14 01:41Z (token kept
  in a shell variable, never printed) has `"five_hour":null`, a `seven_day`
  object, and `limits[]` holding only `weekly_all` and `weekly_scoped` (Fable).
- **The live record showed it.** Before this sprint's publish, `claude:limits`
  read `five_hour_pct 10`, `five_hour_resets_at 1795089599`, which were exactly
  the seven-day values, and had no `five_hour_none`.
- **The contract exists.** kdashdata CD-29 (`docs/architecture.md`, sprint 024)
  and the registry table say what to write.

## What shipped

`publisher/claude-pub.sh`, publisher **2.5.0**:

- **Every object is tested between its two expansions**, in both paths, and for
  `seven_day` as well as `five_hour`. A missing object now leaves its fields
  empty and never reads into another object.
- **oauth**: a `five_hour` object means a window (`P_FHN=0`). A literal
  `"five_hour":null` means no window (`P_FHN=1`). A body that names
  `five_hour` neither way is a shape we do not know, so the poll returns 1 and
  keeps the last value, as the script already does for every unknown shape.
- **statusline**: a `rate_limits` with no `five_hour` object (omitted or null)
  means no window.
- **history file**: a sample with no `fh` means no window, as CD-29 says.
- **The write follows CD-29.** Every headline HSET states `five_hour_none`. A
  no-window write puts `five_hour_none 1`, `five_hour_pct 0` and
  `five_hour_resets_at 0` in the same HSET as `updated_at`, not behind the
  `[ -n "$P_FHR" ]` conditional. That is the one exception to "a source that
  does not know the reset leaves it alone". A window write puts
  `five_hour_none 0` beside the real values, and its reset is written only if
  known, as before. A writer that loses the `updated_at` guard writes nothing.

Tests: `publisher/tests/batch-shape.sh` §15, 12 new assertions. They pass on the
fix, and 9 of them fail against the 2.4.0 script.
- The oauth path runs for real against a stub `curl`, which serves
  `fixtures/oauth/usage-five-hour-null.json`. That body was captured byte for
  byte from the API on kai. Its sibling `usage-five-hour-object.json` differs
  only in `five_hour`. The credential file in the test is a fake written by the
  test.
- The statusline path is tested with the object omitted, with it null, and with
  it present.
- The history file is tested with `fh` and without it.

Docs: `publisher/README.md` lists `five_hour_none` in the `claude:limits` contract
table. `docs/deploying.md` has a 2.5.0 entry and the cross-version gap.

## Where the script runs, and how 2.5.0 reaches each

From k-homelab `manifests/*.yml`, read through kaed on 2026-11-14:

| host | what runs it | how the fix arrives |
|---|---|---|
| kai | `claude-hooks` (statusline + hooks) **and** `kdeskdash-poll` (the oauth poll timer, the only one in the fleet) | `kdeskdash_publisher_version` bump in k-homelab `manifests/common.yml`, then `bin/apply kai claude-hooks` and `bin/apply kai kdeskdash-poll` |
| kubs0 | `claude-hooks` | the same pin, then `bin/apply kubs0 claude-hooks` |
| komarchy | `claude-hooks` | the same pin, then `bin/apply komarchy claude-hooks` (it is a laptop, so this needs it to be awake) |
| kimac | `claude-hooks` | the same pin, then `bin/apply kimac claude-hooks` |
| kbook2 | `claude-hooks` | the same pin, then `bin/apply kbook2 claude-hooks` |
| thrall | `claude-hooks` | the same pin, then `bin/apply thrall claude-hooks` |
| cleo | statusline + hooks, plus the poll scheduled task on the **file** path | unmanaged: hand-copied from the store bundle to `C:\Users\kenhi\.claude\kdeskdash-pub\claude-pub.sh` |

The publish is `just publish-publisher` from merged `main`, which `.sprint-deploy`
already declares.

## Live check, before the ship

At 2026-11-14 01:49Z the branch's script ran `poll` once on kai, not under
`bash -x`. The record read back `five_hour_none 1`, `five_hour_pct 0`,
`five_hour_resets_at 0`, `seven_day_pct 11`, `source oauth`, `host kai`,
`scoped_model Fable`, `scoped_pct 12`. kai's installed 2.4.0 timer writes again
within five minutes. That write copies the seven-day values back in and leaves
the flag at 1 beside them, which is the CD-29 gap. New readers hide the value.
The gap closes when the deploy above lands.
