# Sprint 049 — Codex usage publisher writing `codex:limits`

**Proposal:** korg:4948 (slice 8 of program korg:4951, "Codex as a first-class
homelab agent").
**Covers:** WI 4939.
**Branch:** `049-codex-usage-publisher`, off `94cdc3f`.

Run as a karc leg on kai (`kdeskdash-f68206`), overseen. The contract arrived
first: kdashdata CD-33 (`contracts/schemas/codex-limits.schema.json`, `6381ec3`)
and `kdash-pub 0.1.0-6381ec3`, which already accepts `codex:limits`. This
sprint is the writer.

## Premise check

Checked on 2026-11-24 at about 02:00Z.

- **4939 holds.** No Codex publisher, key or reader existed in this repo. The
  contract named in "depends on the kdashdata contract" is live, and kai's
  `kdash-pub` is the build that accepts the namespace.
- **The app-server source holds, measured.** `codex app-server` on
  codex-cli 0.171.0 answers `account/rateLimits/read` in about 0.75 s with no
  session running. On Ken's account it returns `planType: prolite`, `primary`
  = `{usedPercent 0, windowDurationMins 10080, resetsAt 1796175971}` and
  `secondary: null`. That is the Pro shape the WI predicted: the weekly
  window sits in the primary slot, and there is no five-hour window.
- **The rollout source agrees.** The newest rollout's last `token_count`
  event, from a session that ended at 01:53Z, carried the same snapshot in
  snake_case, with the same reset stamp and 0.0%.

## Decisions

- **A sibling script, not a fourth mode in `claude-pub.sh`.** `codex-pub.sh
  poll` sits next to `claude-pub.sh` the same way `ghcp-pub.sh` does: the same
  transport and guard, and a different source and contract. It ships in the
  same bundle on the same version clock (2.7.0), with its own poll unit
  pair.
- **Writer host: kai.** Limits are account-wide, so there is one writer. kai
  avoids WI 4651, which says cleo has no release path for this bundle. That
  matches the overseer's note on the proposal.
- **App-server first, rollout fallback.** The rollout sample is published
  with its **own** timestamp as `updated_at`, so an old rollout reads as
  stale. The guard publishes only a sample *newer* than the stored
  `updated_at`, which covers CD-33's "must not advance on a republish". It
  also stops a rollout from replacing a fresher app-server reading.
- **`source` values: `app-server` and `rollout`.** CD-33 names `rollout` and
  allows others.
- **No 10080-minute window means an incomplete sample, so nothing is
  published.** The schema requires `seven_day_*`, and CD-33 says an
  incomplete sample leaves the last one to age. A 300-minute window with no
  weekly window has not been observed on any plan.
- **Fields not written:** `limitId`, credits and `rateLimitReachedType`,
  which CD-33 leaves out. Nothing here needed them, so there is no contract
  change to raise.
- **Stem `KDASH_CENTRAL_REDIS`**, from the registry's row for the family.

## What shipped

- `publisher/codex-pub.sh`. It runs the JSON-RPC exchange as a `coproc`,
  because the app-server **exits on stdin EOF before answering**: a plain
  pipe gets the `initialize` reply and nothing else (measured). Windows are
  mapped by `windowDurationMins`/`window_minutes`. Every required field goes
  out in one HSET, plus `plan_type`. The script reads no `auth.json`, and it
  leaves breadcrumbs in `~/.codex/kdeskdash-pub/state/`.
- `publisher/kdeskdash-codex-poll.{service,timer}`, which run every 5 minutes
  (`expected_refresh_s` 300) with ExecStart at
  `%h/.codex/kdeskdash-pub/codex-pub.sh poll`.
- `publisher/tests/codex-batch-shape.sh` (ctest `test_publisher_codex`), 11
  cases. Its stub `codex` exits on EOF without answering, as the real one
  does. The fixtures are the real kai exchange with ids zeroed. The plus-plan
  fixture is derived, and its by-id copy deliberately disagrees, so a parser
  that reads the wrong snapshot fails.
- The bundle payload (`publish-publisher.sh`, `version-publisher.sh`) and
  `publisher/VERSION` 2.7.0.
- Docs: `publisher/README.md` (new section), `docs/deploying.md` (bundle
  list, 2.7.0), and the README tree.

### A bug the live run caught

The first logged-out run against the live hash wrote nothing, which was
correct, but its breadcrumb said "no answer". The real error line is
`{"error":{…},"id":2}`, with `id` **last**, and the reader matched only lines
starting `{"id":2,`. The hand-built logged-out fixture had the friendlier key
order, so the test passed against a reply the server never sends. The fixture
was recaptured from the real server, the test went red, and the matcher was
fixed.

## Acceptance (triggered, same session, probed from kai)

- **"Tracks Codex's own usage display within one refresh."** The hand-run at
  `1795572266` published `seven_day_pct 0`, `seven_day_resets_at 1796175971`,
  `five_hour_none 1`, `plan_type prolite`, `source app-server`. That matches
  the newest rollout `token_count` (0.0%, the same reset), which is the
  snapshot Codex's own status display renders. The weekly window had opened
  at 01:46Z, so the reading is a real zero rather than a default. The record
  validates against `codex-limits.schema.json` (Draft 2020-12, `jsonschema`
  4.10.3) once its hash strings are decoded.
- **"Logged out reads stale or absent, not zero."** A run with a scratch
  `CODEX_HOME` (never Ken's login, and `auth.json` was never read) left the
  live `updated_at` at `1795572266`. The breadcrumb named the auth error. The
  stored reading therefore ages, and the panel greys it at `updated_at + 300
  + grace`.

## Not done here, on purpose

- **No persistent install.** The proposal's notes require Ken's OK in the
  moment for a new timer, and k-homelab has no recipe for this:
  `kdeskdash-poll` hardcodes `claude-pub.sh` in its paths and its pairing
  guard. The proposed install and its fold-in are in the overseer handoff.
- **No store publish yet.** The bundle publishes from merged `main` at ship.

## Repaired in passing

None.
