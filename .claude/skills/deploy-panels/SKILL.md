---
name: deploy-panels
description: Publish kdeskdash from committed main to the homelab package store, install that published version on each dashboard Pi, and verify every reachable board is running that exact build. Use when asked to deploy/redeploy/ship kdeskdash to the panels, or when sprint-ship reaches Phase 7. Deploys committed code only.
---

# Deploy kdeskdash to the panels

**Publish once, install that.** `just publish` puts a versioned artifact in the
homelab package store; `just deploy <host> <version>` installs *that artifact*
on a board. The thing you verified is the thing the panel runs, because it is
the same bytes fetched — not the same commit rebuilt.

This replaced copying the binary out of `build-pi/` in sprint 024 (korg #1016),
the last such deploy in the fleet. Doctrine: k-homelab `docs/deploying.md`; the
kdeskdash-specific half is `docs/deploying.md` here.

## The fleet

| Host | Board | Where | Notes |
|---|---|---|---|
| `rpidash2` | Pi 5 | dev desk, beside kai | the default target; reliably reachable |
| `rpidash3` | Pi 4 | work desk | **frequently off or off-network** |

Both run the same generic-aarch64 build as user `ken`; what differs is
`deploy/hosts/<host>.env` (mode set, endpoints), which deploys never touch.

## The Pis are unmanaged, and that shapes everything here

They are not on the tailnet. They hold no store URL, no credentials, nothing
that has to be kept current on a device bolted under a desk whose recovery
story is carrying a keyboard to it. So:

- **The dev box does the fetching.** `just deploy` resolves the version,
  fetches, and verifies checksums *on kai*, then pushes over ssh. There is no
  `--from-store` mode to run on a board, and a board cannot update itself.
- **Deploys start from the clone on kai.** There is nowhere else to run this.

### What this skill can and cannot assert

Say this plainly in the report rather than implying a clean sweep.

**Can assert**, per *reachable* board: the artifact was fetched intact
(`SHA256SUMS`), the installed binary reports exactly the published version, the
unit is active, and — via `kddss` — the running app rendered a frame.

**Cannot assert** anything at all about an unreachable board. And a screenshot
proves the app is drawing, not that what it drew is *right*; if the sprint
changed a layout, look at the PNG.

**An unreachable board is a normal outcome, not a failed deploy.** rpidash3 is
at the work desk and is often simply off. Report it as pending and move on — do
not retry-loop, do not go hunting for it, and do not treat the fleet as broken.
The boards are independent: no shared state, no schema, nothing that couples
them, so half-deployed is a perfectly stable place to sit for days. Note the
pending board in the sprint record so the next deploy picks it up.

## Publish from clean, committed `main` — never a branch

`just publish` refuses a dirty tree and refuses a version stamped `-dirty` or
`unknown`: a published version must name a commit, or it is a rollback target
nobody can reproduce.

It also **will not move `latest`** off `main`. A branch commit disappears from
history at squash-merge, leaving a board reporting a SHA that is on no branch.
This is why sprint-ship deploys in Phase 7, *after* the merge — publishing from
merged `main` is what keeps every store version's commit an ancestor of
`origin/main`. Preserve that ordering.

## Procedure

Run from the checkout on kai (`~/src/tools/kdeskdash`).

### 1. Publish from merged main

```sh
git -C ~/src/tools/kdeskdash status --short              # must be empty
git -C ~/src/tools/kdeskdash rev-parse --abbrev-ref HEAD # must be main
git -C ~/src/tools/kdeskdash pull --ff-only origin main
cd ~/src/tools/kdeskdash && just publish
```

Stop and ask if the tree is dirty or the branch is not `main`. Never stash.

`publish` prints the version — capture it and pin every board to it explicitly
rather than letting each resolve `latest` on its own:

```sh
V=0.24.0-<sha>          # exactly what publish printed
```

Requires `KDESKDASH_STORE_URL` and `KDESKDASH_STORE_HOST` in `.env` (gitignored;
see `docs/deploying.md`). Neither has a default — if publish complains about
one, set it, do not guess it.

### 2. Install on each board — rpidash2 first

rpidash2 is beside the dev box, so if something is wrong you find out where you
can see the screen.

```sh
just deploy rpidash2 "$V"
just deploy rpidash3 "$V"     # expect this to fail on connect, often
```

`deploy` stops the service, installs to `/usr/local/bin/kdeskdash`, sends the
Nerd Font only when its checksum differs, **asserts the installed binary reports
`$V`**, and restarts. It fails loudly rather than half-installing.

An unreachable board fails at ssh with a connection error. That is the expected
outcome for rpidash3 much of the time — record it, do not chase it.

> **First deploy to a board still on a pre-024 binary**: the version probe
> pauses ~10 s and reports the board as "too old to say" before installing.
> That is the transition, once per board. A pre-024 binary has no `--version` —
> it ignores the argument and starts the dashboard — which is why the probe is
> bounded by `timeout -k`. Do not remove that bound.

### 3. Verify each reachable board

```sh
just versions
```

Reports the store's versions, what is cached on kai, and what each board is
running. Assert the board line equals `kdeskdash $V` exactly — do not eyeball
it. Then confirm the unit and take a frame:

```sh
ssh ken@rpidash2 'systemctl is-active kdeskdash'
KDD_HOST=ken@rpidash2 scripts/kddss deploy-$V
```

`kddss` is the strongest check available for a panel: it round-trips through the
control Redis and the running LVGL app and comes back with what is actually on
the screen. A unit being active only proves *a* kdeskdash is running; a rendered
frame proves this one is drawing. If the sprint changed anything visual, open
the PNG and look at it.

### 4. Report

A line per board: version installed, `--version` output, unit state, screenshot
taken or not. Name every board that was not reached, and say explicitly that
nothing is asserted about it.

## Rollback

A bad deploy does not roll back a merge — the code landed fine, the rollout
didn't. **Naming an older version is the whole rollback; there is no second
verb**, and it is the capability the old scp flow never had:

```sh
just versions                          # what exists in the store
just deploy rpidash2 0.24.0-<older>    # this is the rollback
```

The store's history *is* the rollback path, and it survives kai losing its
`.deploy-cache/` (that cache is only a fetch optimisation — a version that
verifies is reused, anything else is re-fetched). Published versions are
immutable, so an old version is exactly the bytes that worked.

Roll back only the board that is wrong. A split fleet is a normal state here.

## What this skill does not do

- **Device config.** `deploy/hosts/<host>.env` → `/etc/kdeskdash/kdeskdash.env`
  is `just install-service`, one-time per device, and it never overwrites an
  installed file. A deploy does not touch it.
- **Secrets.** `/etc/kdeskdash/secrets.env` (`KVSCF_TOKEN`,
  `KDESKDASH_TELEMETRY_REDISCLI_AUTH`) is hand-installed once per device at mode
  0600 and never committed — `deploy/hosts/README.md`.
- **The systemd unit.** Shipped inside the artifact, but installed only by
  `just install-service <host> [version]`. If a sprint changed
  `deploy/kdeskdash.service`, run that on **every** board — the unit is the one
  thing that drifts a fleet one device at a time
  (`docs/solutions/best-practices/systemd-sandboxing-needs-a-second-device.md`).
- **The claude-feed Redis instance** (`deploy/redis-claude.*`, port 6380) and
  the publisher hooks on other machines. Separate installs, separate lifecycles.
