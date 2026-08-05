# One Redis key, several independent writers

**The worked example:** `claude:limits` on the claude-feed Redis. Three
publishers write it — the Claude Code statusline hook (sub-minute, but only
while a session renders), a Windows scheduled task reading the desktop app's
`plan-usage-history.json` on cleo (5 min, percentages only), and a systemd
user timer calling the OAuth usage endpoint on kai (5 min, the only source of
reset stamps and the model-scoped weekly window). Different hosts, different
cadences, different subsets of the fields — one hash. Sprint 023;
`publisher/claude-pub.sh` and `publisher/README.md` carry the concrete
contract.

## The rules, in dependency order

1. **Stamp observations, not publishes.** `updated_at` is when the data was
   *observed* (a desktop-app file sample can be 5 minutes old when read), not
   when it was written. Publishing "now" makes a lagging source look fresher
   than a live one it should lose to.

2. **Last observation wins, enforced by read-back.** A polling writer HGETs
   the stored stamp first and refuses to publish over a fresher observation.
   This is what lets a live statusline on *any* host always beat the pollers,
   with no coordination between writers. (Safe here because the quota is
   account-global — the writers are redundant observers of the same value,
   not owners of different values.)

3. **A writer only sets fields it can supply.** The file source cannot know
   reset timestamps, so it leaves `*_resets_at` alone. Never write `0` or any
   sentinel for "unknown" — the reader can't tell it from data, and it
   destroys a good value some other writer supplied.

4. **Fields whose availability differs by source get their own stamp.** This
   is the rule the others don't imply, and skipping it is invisible: a file
   write is *newer* than the OAuth write, wins rule 2, refreshes `updated_at`
   — and cannot touch the `scoped_*` fields it doesn't know. Under one shared
   stamp the scoped gauge reads as fresh while frozen, indefinitely. So the
   scoped set carries `scoped_updated_at`, and its writer guards it with its
   own read-back (rule 2 applied per stamp), not the headline one.

5. **Writers publish their own cadence; readers grey on stamp + cadence.**
   Each writer sets `expected_refresh_s` for every field-set it touches; the
   panel greys a gauge when `now - stamp > expected_refresh_s + grace` and
   needs zero knowledge of which sources exist. Policy lives with the thing
   that knows its own cadence. A field-set with data but no stamp renders
   stale from the start — it must never borrow another set's freshness.

## What this buys

Stop every writer and each gauge greys on its own schedule, still legible;
restart any writer and the gauges it feeds come back — self-healing with no
"which source is authoritative" switch anywhere. Adding a fourth writer is a
new cron entry, not a code change on the reader.

## Testing it

The whole contract is exercisable offline: a ~60-line fake RESP server (dict
+ HGET/HSET) and mocked sources. The three cases that matter: a full write
from the richest source; a *fresher* write from a poorer source (must move
its fields, must not touch the richer set or its stamp); a *staler* write
(must be refused entirely). Sprint 023's record has the harness.
