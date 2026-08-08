# 024 — Pi builds ship from the package store

WI #1016 · proposal korg:1025 · slice 5 (last) of program korg:1026 "Deploy from the store"

## Goal

`just deploy` cross-compiled and scp'd the binary straight out of `build-pi/`
onto a board. That is an artifact-shaped deploy with no artifact: no history, no
record of what a panel is actually running, and — the one that matters — no
previous build to fall back to when a deploy is bad, on devices that are
genuinely annoying to debug in place. Convert to the homelab doctrine
(k-homelab `docs/deploying.md`): publish a versioned artifact to the store,
deploy *that*.

kdeskdash is the program's cross-compile case, so the shape here is the one a Go
binary or a future embedded target inherits.

## What shipped

- **[VERSION](../VERSION) + [scripts/version.sh](../scripts/version.sh)** — the
  one place a version is derived: `<VERSION>-<short commit>[-dirty]`, minor
  tracking the sprint number. Every commit is a new version, so publishing never
  needs a bump of its own.
- **`kdeskdash --version`** ([src/main.c](../src/main.c)) — stamped by the build
  recipe via `-DKD_VERSION`, answered before DRM or evdev is touched.
- **[scripts/publish.sh](../scripts/publish.sh)** (`just publish`) — refuses a
  dirty tree, cross-compiles, stages binary + font + unit + env example +
  installer, hands them to `kpkg` on the store host. Off `main` it publishes
  without moving `latest`.
- **[scripts/deploy.sh](../scripts/deploy.sh)** — rewritten around the store:
  `deploy <target> [version]` resolves `latest` or a named version, fetches into
  `.deploy-cache/`, verifies `SHA256SUMS`, pushes, and asserts the installed
  binary's version. `install-service` now takes its unit from the artifact.
  `versions` reports published / cached / running. `push-dev` is the dev loop.
- **[.sprint-deploy](../.sprint-deploy) + the
  [`deploy-panels` skill](../.claude/skills/deploy-panels/SKILL.md)** — the
  declaration that makes `/sprint-ship` Phase 7 publish from merged `main` and
  roll the boards, instead of the deploy being a thing someone remembers to run.
- **[docs/deploying.md](../docs/deploying.md)** — the kdeskdash-specific half of
  the doctrine; README's build-and-deploy section rewritten around it.
- **Retired**: the `deploy` / `install-service` CMake custom targets and the
  `KDESKDASH_TARGET` cache variable. They were the copy-from-build-tree path
  under a second name, and leaving them would have left the old habit reachable.

## Decisions

**The dev box fetches, not the board.** Every other slice has the target host
pull its own artifact. The Pis stay unmanaged — no tailnet, no store URL, no
credentials, nothing on a device bolted under a desk that has to be kept
current. So `deploy` resolves, fetches and verifies on the dev box and pushes
over the ssh that was already the deploy path. The cost is honest and worth
naming: a board cannot update itself, and deploys start from a clone.

**`--version` on the binary, because a health check proves nothing.** The old
process answers a health check exactly as well as the new one, and on a panel
that is *especially* convincing — the screen keeps showing something plausible.
Asking the installed binary what it is compares the fetched version against what
is on disk; the checksum already proved the fetch was what it claimed. This is
kfdc sprint 005's `readlink /proc/<pid>/cwd` lesson in the form a single static
binary can carry.

**The version probe has to be bounded, and finding out why cost a stray
process.** The first probe against rpidash2 hung: a pre-024 binary has no
`--version`, ignores the unknown argument, and *starts the dashboard* — so the
check that exists to catch a failed install is precisely the case that hangs on
one, leaving a second instance fighting the service for the panel. It also would
not die of `SIGTERM`, blocked in DRM init. The probe now runs under
`timeout -k 2 10`. Worth generalizing: a version probe is only diagnostic if the
thing that cannot answer *fails*, and a binary that ignores unknown arguments
does the opposite of failing.

**The deploy declaration ships in this slice, not as a follow-up.** The overseer
comment on korg:1025 caught this mid-sprint: korg, klams, kaed and kwebi all
carry a root `.sprint-deploy`, kfdc's slice shipped without one, and that cost
kfdc a hand-run deploy plus its own work item afterwards (kfdc #1035). The skill
states plainly what it can and cannot assert, because with these targets that is
not boilerplate — see below.

**An unreachable board is a normal outcome, not a failed deploy.** rpidash3 sits
at the work desk and is often simply off; the boards share no state, no schema
and nothing that couples them, so a half-deployed fleet is stable for days. A
deploy skill that treats that as breakage would train everyone to ignore it, so
`deploy-panels` says to record the pending board and move on. Verification says
what it covers too: a rendered frame via `kddss` (the control-Redis round trip
proves the running app is *drawing*, which `systemctl is-active` does not) for
reachable boards, and nothing whatsoever asserted about one that was not reached.

**CMake never derives the version.** The recipe passes it in. A cached CMake
variable freezes at whatever it was configured with, and a stamp that lies about
the commit is worse than no stamp — a hand-configured build stamps `unknown`,
which `deploy` refuses to install.

**The font ships in every artifact.** 2.4MB, byte-identical release after
release, and a real argument for versioning it separately — which loses to the
property that one version directory can put a board back the way it was. The
wire cost is paid once: `deploy` compares checksums and only sends it when it
differs. Per-host env files go the other way and deliberately do *not* ship:
device config on its own clock, never overwritten, repo is the source of truth.

**`push-dev` is a deliberate, labelled exception.** kdeskdash cannot run on the
dev box at all — no DRM panel, no touch — so the board is the only place a
change can be seen, and a commit + publish per glance at a layout tweak would
make iterating absurd. The doctrine draws its line at deploys and puts dev
iteration on the other side. What keeps it honest is the stamp: a dirty tree
builds `-dirty`, so `just versions` reports a board running something that is
not in the store instead of quietly implying it is.

## Verification

Live, against the real store and rpidash2:

- **Publish** — `just publish` twice, `0.24.0-9f8acf9` and `0.24.0-185fe20`
  (3.9MB each). Off `main`, so `latest` was correctly left unset; `kpkg list`
  shows `(latest: ?)`.
- **Deploy** — both versions installed on rpidash2, service back to `active`,
  `kdeskdash --version` answering from `/usr/local/bin/kdeskdash`.
- **Rollback** — `just deploy rpidash2 0.24.0-9f8acf9` after the newer one, then
  forward again. The second and third deploys reused the verified local cache
  rather than re-fetching. This is the capability the scp flow never had.
- **Failure paths** — an unpublished version fails at the fetch with a sentence,
  not a curl error; with no `latest` (branch-only publishes) `just deploy`
  declines and says to name a version; `just versions` reports store / cached /
  per-board, with rpidash3 as unreachable.

**rpidash3 is still on its pre-024 binary** — it was off-network the whole
sprint (work desk). It needs one `just deploy rpidash3 <version>` when it is
next reachable; nothing about that is expected to be interesting, but until then
the fleet is one board converted and one not.

## Deployed 2026-08-06

First run of the `deploy-panels` skill, in sprint-ship Phase 7 — so the artifact
was published from the merged squash commit, not from the branch.

- **Published** `0.24.0-6f819b7` (3.9MB). Publishing from `main` moved
  `latest` for the first time, which is the property the branch publishes
  deliberately withheld.
- **rpidash2** — installed, `kdeskdash 0.24.0-6f819b7` asserted exactly against
  the published version, unit `active`, and `kddss` returned a rendered frame
  (Claude mode drawing agents + usage gauges).
- **rpidash3 — NOT deployed, pending.** Off-network for the whole sprint. The
  attempt failed at ssh *after* fetching and verifying the artifact, so nothing
  was half-installed. It still runs its pre-024 binary, and expect the ~10 s
  "too old to say" probe pause on its first store deploy. **Nothing is asserted
  about that board.** Rollback target for rpidash2 if needed:
  `just deploy rpidash2 0.24.0-185fe20`.

## Deployed 2026-08-08 — rpidash3, fleet uniform

rpidash3 came back on-network once the case-print fit testing finished (WI
#1036). No new publish: the only commit past `0.24.0-6f819b7` is the
`.deploy-cache` fix below, which touches `scripts/deploy.sh` — host-side, not
shipped in the artifact — so installing the *same* published version is what
makes the fleet uniform rather than splitting it again.

- **rpidash3** — `just deploy rpidash3 0.24.0-6f819b7`, installed from the
  verified local cache (no re-fetch). `kdeskdash 0.24.0-6f819b7` asserted
  exactly, unit `active` on a fresh PID, and `kddss` returned a rendered frame
  (Clock mode, local + UTC + stopwatch, times correct).
- **The unit did not drift.** `deploy/kdeskdash.service` is byte-identical
  (md5 `6987f09…`) across the committed copy, rpidash2, and rpidash3, and
  `/etc/kdeskdash/kdeskdash.env` was already the sprint-019 mode set — so no
  `install-service` was owed on this board.
- **The pre-024 probe hazard fired exactly as documented and was harmless**: a
  `just versions` *before* the deploy started a second instance on rpidash3 that
  lost DRM master and logged `drmModeAtomicCommit: Permission denied`. The
  `timeout -k 2 10` bound reaped it; the service's own process was untouched.
  Confirmation that the bound is load-bearing, not belt-and-braces.

Both boards now run `0.24.0-6f819b7`. Every deploy in the fleet is store-served.

## Follow-ups

- **The first publish from `main` happens in sprint-ship Phase 7**, via
  `deploy-panels`. Until then `latest` is unset by design (branch publishes do
  not move it), so `just deploy` with no version has nothing to resolve.
- ~~**rpidash3 needs its first store deploy**~~ — done 2026-08-08, see above.
  Phase 7 attempted it and it was offline, as expected.
- ~~**A failed fetch leaves an empty `.deploy-cache/<version>/` behind**~~ —
  fixed in `746da99` (#30). It
  then shows in `just versions` as if that version were cached. Found by this
  sprint's own negative test (`0.99.0-nope`). Cosmetic, but a cache listing that
  names a version it does not have is exactly the kind of small lie this sprint
  spent its effort removing. One-line fix: clean the directory before `die`.
- Show the version on-panel (menu footer or Dev mode) — the board can already
  answer over ssh, but not to someone standing in front of it.
- `just versions` hardcodes the two known boards; the fleet roster living in one
  place (it is already implicit in `deploy/hosts/`) would be tidier.
