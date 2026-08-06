# Deploying kdeskdash

kdeskdash ships from the homelab package store. `just publish` puts a versioned
artifact in it; a board gets *that artifact*, checksum-verified, over ssh.

This is the homelab-wide doctrine — k-homelab `docs/deploying.md` is the
authority, and kdeskdash was the last service still copying a binary straight
out of a build tree. The general shape (store on kubsdb `:4880`, `kpkg` to
publish, `latest` pointer, immutable versions, nightly mirror to the NAS) is
documented there and not repeated here. What follows is specific to kdeskdash.

## Setup, once per dev box

Two variables in `.env` (gitignored, beside `KVSCF_TOKEN`). Neither has a
default in the scripts, on purpose: a guessed hostname fails later as a
confusing `curl` error instead of here as a sentence.

```sh
KDESKDASH_STORE_URL=https://kubsdb.encke-wahoo.ts.net:4880
KDESKDASH_STORE_HOST=kubsdb     # publisher only — the host running kpkg
```

## The commands

```sh
just publish                          # release: build + publish a version
just deploy [host] [version]          # install a published version on a board
just deploy rpidash2 0.24.0-1a2b3c4   # ...an older one. This is the rollback.
just install-service [host] [version] # unit (from the artifact) + host env
just versions                         # published / cached here / running where
just push-dev [host]                  # dev loop — NOT a deploy, see below
```

## Why the dev box does the fetching

Every other slice of the store rollout has the target host fetch its own
artifact. The Pis do not, and that is deliberate: they are **unmanaged**. No
tailnet membership, no store URL, no credentials, nothing on the device that has
to be kept current — a dashboard whose recovery story is "carry a keyboard to
the desk it is bolted under" earns a minimal surface. So `just deploy` resolves
the version, fetches, and verifies **on the dev box**, then pushes over the ssh
that was already the deploy path.

The consequence worth knowing: a board cannot update itself, and there is no
`--from-store` mode to run on one. Deploys start from a clone.

## Versions

`scripts/version.sh` is the only place a version string is derived:

```
<VERSION file>-<short commit>[-dirty]        e.g. 0.24.0-1a2b3c4
```

The minor tracks the sprint number — bump `VERSION` in the sprint that changes
what ships. The commit means **every commit is a new version**, so publishing
never needs a version bump of its own (`kpkg` refuses to overwrite a published
version, and this is how that rule stays out of the way).

The recipe passes the version to CMake (`-DKD_VERSION`); CMake never derives it.
A cached CMake variable freezes at whatever it was configured with, and a stamp
that lies about the commit is worse than no stamp at all. A build configured by
hand with no flag stamps `unknown`, and `just deploy` refuses to install one.

`publish` refuses a dirty tree, and off `main` it publishes **without** moving
the `latest` pointer — a branch commit vanishes from history at squash-merge, so
a branch build may exist in the store to prove a path but must never be what the
fleet gets when it asks for the newest.

## What is in an artifact

`artifacts/kdeskdash/<version>/`, flat, plus `SHA256SUMS`:

| file | why |
|---|---|
| `kdeskdash-aarch64-linux` | the binary, named with the target triple so a second architecture can sit beside it |
| `SymbolsNerdFont-Regular.ttf` | the icons mode is unavailable without it, so a version that cannot restore it cannot restore a board |
| `kdeskdash.service` | the unit a build was released with, which is the unit that build should run under |
| `kdeskdash.env.example` | the full-surface config reference as of that build |
| `deploy.sh` | the installer, so a version's push logic is recoverable with it |

The font is 2.4MB and byte-identical release after release, which is a real
argument for versioning it separately — and it loses to the property that one
version directory can put a board back exactly the way it was. The wire cost is
paid once: `deploy` compares checksums and only sends the font when it differs.

**Per-host env files deliberately do not ship.** `deploy/hosts/<host>.env` is
device config on its own clock, `install-service` never overwrites an installed
one, and the repo is its source of truth.

## Proving a deploy took

A health check cannot do it. The old process answers just as well — and on a
panel that is *especially* convincing, because the screen keeps showing
something plausible either way.

So `kdeskdash --version` exists, handled before DRM or evdev is touched (it has
to answer over ssh on a board whose panel is owned by the running instance).
After installing, `deploy` asks the binary on disk what it is and fails loudly
unless it matches the version that was fetched. The checksum proved the transfer;
that proves the label.

**The probe runs under `timeout -k`, and that is load-bearing.** A binary from
before sprint 024 has no `--version`: it ignores the unknown argument and
*starts the dashboard*. So the probe that exists to detect a failed install is
exactly the case that would hang on one — and leave a second instance fighting
for the panel. It will not die of `SIGTERM` while blocked in DRM init either,
hence the `-k` `SIGKILL`. On a board that has never had a 024 build, expect
`just versions` to pause ten seconds and then report it as too old to say; that
is the transition, once per board.

## `push-dev` — the labelled exception

kdeskdash cannot run on the dev box at all: no DRM panel, no touch. The board is
the only place a change can be looked at, and requiring a commit and a publish
per glance at a layout tweak would make iterating absurd. k-homelab's doctrine
draws its line at *deploys*, and dev iteration is explicitly on the other side.

`just push-dev [host]` cross-compiles this tree and pushes it to a board through
the same code path a deploy uses. What keeps it honest is the stamp: a dirty
tree builds a `-dirty` version, so `just versions` reports a board running
something that is not in the store rather than quietly implying it is.

Anything that stays on a panel should be published and deployed.
