# Sprint 029 — publish the claude-feed publisher to the package store

korg:1325 (proposal) / #1323 (work item). Slice 3 of program korg:1309, paired
with k-homelab #1324 — publish first, consume second, one slice apart on
purpose. Nothing on any host changes until #1324 lands; this artifact ships
and sits unused.

## Goal

k-homelab was getting `publisher/claude-pub.sh` and the two poll units across
machine boundaries two interim ways: a vendored copy (went eleven days stale,
would have broken kai's poll timer — k-homelab #1313) and staging from kai's
checkout (staleness traded for a checkout dependency). The homelab's own
doctrine already covers this case — a built bundle goes to
`artifacts/<name>/<version>/`, cross-machine consumption goes through the
store — it just was never routed through it. This sprint adds the publishing
half: `just publish-publisher` → `artifacts/kdeskdash-publisher/<version>/`.

## The version decision

The proposal's one open question: the publisher has no version, and the store
refuses a version it has already served. The constraint is that the version
must move whenever `claude-pub.sh` or a unit does, or `latest` silently serves
stale content — the exact bug this sprint exists to kill, rebuilt one layer
down.

**Chosen: `<publisher/VERSION>-<short sha of the last commit touching the
payload>`** (e.g. `1.0.0-47f7c49`), payload = the script, the two units, and
`publisher/VERSION` itself. Why over the alternatives:

- **Date stamp** — moves when you *publish*, not when the content changes.
  Ties the version to nothing, and can mint distinct versions of identical
  content.
- **Riding the repo tag / dashboard version** — mislabels the bundle with the
  dashboard's release train and mints a new publisher version on every commit,
  payload changed or not.
- **Payload-scoped sha** — the version moves exactly when the shipped content
  does, so a stale `latest` after a publisher change is structurally
  impossible; and republishing an *unchanged* publisher reproduces the version
  the store already holds, whose refusal is correct information ("already
  published"), not an obstacle. Same shape as the dashboard's
  `<VERSION>-<sha>`, so it reads as one convention. The `publisher/VERSION`
  base (starting at 1.0.0 — the script and its `claude:limits` contract are
  fleet-deployed and stable) is the human intent track; forgetting to bump it
  is harmless because the sha moves regardless.

The bundle also carries a generated `VERSION` file with the full string, so an
installed copy can answer "what is on this host" without the store path that
delivered it.

## What shipped

- `scripts/publish-publisher.sh` — stages the three payload files + generated
  `VERSION`, publishes via `kpkg artifact` on the store host. Same guardrails
  as `publish.sh`, same reasons: refuses a dirty tree, off-`main` publishes
  without moving `latest` (squash-merge erases branch shas).
- `publisher/VERSION` — the base, `1.0.0`.
- `just publish-publisher` recipe.
- Docs: `docs/deploying.md` gains "The publisher bundle" (second artifact, own
  clock); `publisher/README.md` points managed-host installs at the store;
  CLAUDE.md command list.

Path proven from this branch with a `--no-latest` publish (doctrine-blessed:
a branch build may exist in the store to prove the path).

## Follow-ups

- k-homelab #1324 consumes the bundle and retires both interim mechanisms.

## Deployed 2026-08-16

Shipped as PR #36, squash `0d6a98d`.

- **Publisher bundle**: `kdeskdash-publisher 1.0.0-0d6a98d` published from
  main; `latest` created and verified over the store URL (it did not exist
  before — the branch proof was `--no-latest` on purpose). This is the version
  k-homelab #1324 consumes. Rollback target: none needed — first `latest`.
- **Panels**: `kdeskdash 0.27.0-0d6a98d` published (no binary-facing change
  this sprint — same code as `0.27.0-b316406`, new stamp) and installed on
  **both** boards; `just versions` reports the fleet uniform on it, both units
  active, and `kddss` returned a rendered frame from each. Rollback target:
  `0.27.0-b316406`.
