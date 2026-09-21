# Sprint 040 — Fold rpidash2's pair Redis into central

**Proposal:** korg:2932 (slice 4 of program korg:2935, "Two Redis servers").
**Covers:** WI 2305 (remove the legacy Claude-feed fallback), WI 2882 (README
names kvscf as the publisher).
**Branch:** `040-kvscf-fold-to-central`, off `602771b`.

Overseen sprint: run as karc leg `kdeskdash-2e237a` on kai, reviewed on the
proposal thread, shipped on the overseer's green light.

## Goal

Move the dev desk's kvscf channel — cleo's deck ↔ rpidash2's panel — off
rpidash2's own `:6380` and onto central (`rpi53:6379`), so the program can
retire that server. Leave `redis-claude` running but unused, because k-homelab's
manifest still declares it and kmon's nightly would report a declared service
missing. rpidash3 and kwork are untouched by design.

## What the premise check found, and how it changed the plan

The proposal's step 1 said the auth fallback follows the endpoint, so repointing
would make kvscf inherit the central password. **Measured on rpidash2, that was
wrong**, and it is the finding the sprint is built around.

`config.c` resolved the kvscf password by an *ordered lookup* —
`KVSCF_REDISCLI_AUTH`, then `CLAUDE_REDISCLI_AUTH` — **before** the
endpoint-following branch, which was only the fallback when neither name was
set. A comment said the order "never fires in practice" because exactly one of
the two is set per panel. True, and a premise about the *topology*: each panel
read its own board's instance, so only its own board's key was rendered.

Read out of the running panel's own environment (PID 8586 on rpidash2):

```
CLAUDE_REDISCLI_AUTH   len=56  fp=2a1ebfd180be   redis-claude's :6380 password
REDISCLI_AUTH          len=55  fp=da3db3b2b333   central's
```

Both present, different secrets, and `CLAUDE_REDISCLI_AUTH` must stay until
slice 2934 retires that server. So repointing the endpoint alone would have sent
the `:6380` password to central — and AUTH with a wrong password is an *error*,
not a shrug, so the handle never opens and the panel reports "kvscf feed
unavailable" as though central were down. Sprint 031's failure, one layer over.

That made WI 2305 load-bearing rather than a tidy-up riding along, and set the
shape of the whole slice: **every one of the three things that used to be
inferred is now named by the board.**

## What shipped

### Code

- **`src/config.c` — the kvscf endpoint's fallback is gone** (WI 2305).
  `KDESKDASH_KVSCF_REDIS_HOST`/`_PORT` default to `127.0.0.1:6380`, the pin both
  panels already carried by hand. Nothing is inherited from the Claude feed.
- **`KDESKDASH_KVSCF_REDIS_AUTH_KEY`** — the board names which fleet key holds
  its kvscf password. The ordered lookup survives only as the *unset* path, so a
  device whose env file predates this still authenticates.
- **`KDESKDASH_KVSCF_TOKEN_KEY`** — the board names which fleet key holds its
  pair's token (`KCTRLDECK_TOKEN_CLEO_PAIR` / `_KWORK_PAIR`, k-homelab sprint
  069). `KVSCF_TOKEN` from `/etc/kdeskdash/secrets.env` is the deprecated last
  rung. No shared name and no list spanning both desks: the overseer's ruling on
  handoff korg:2951, and the failure it prevents is silent — a panel holding the
  other desk's token renders every feed normally and does nothing on a tap.
- **`KDESKDASH_KVSCF_PAIR_HOST`** — kdashdata CD-8's obligation on this slice.
  The endpoint used to scope the panel to one workstation for free, because only
  that workstation wrote to it; on a shared server the host segment is all that
  is left. Two pure functions in `src/kvscf_feed.c` own it: `kvscf_scan_match`
  narrows every SCAN from `kvscf:<family>:*` to the pair's exact key, and
  `kvscf_pair_allows` refuses to publish a command anywhere else — the write
  half matters because the command carries the token. A malformed value warns
  and degrades to the wildcard rather than stopping the panel; a pattern that
  would not fit writes nothing, because a truncated pattern matches no key and
  would read as an empty feed rather than as the config error it is.
- `kvscf_redis_init` takes the pair host and logs the resolved pair and
  endpoint at startup — the line a cut-over is verified by.

### Config that must reach the devices

`install-service` never overwrites a board's existing
`/etc/kdeskdash/kdeskdash.env`, so these are **hand edits on the boards at
deploy time**, not something a deploy carries (overseer comment 2674, point 2):

| | rpidash2 | rpidash3 |
|---|---|---|
| `KDESKDASH_KVSCF_REDIS_HOST/PORT` | `rpi53` / `6379` | `127.0.0.1` / `6380` (unchanged) |
| `KDESKDASH_KVSCF_REDIS_AUTH_KEY` | `REDISCLI_AUTH` | `KVSCF_REDISCLI_AUTH` |
| `KDESKDASH_KVSCF_PAIR_HOST` | `cleo` | `kwork` |
| `KDESKDASH_KVSCF_TOKEN_KEY` | `KCTRLDECK_TOKEN_CLEO_PAIR` | `KCTRLDECK_TOKEN_KWORK_PAIR` |

### Docs

`README.md` (WI 2882's two lines, plus the env table), `deploy/kdeskdash.env.example`,
both `deploy/hosts/<host>.env`, `deploy/hosts/README.md`,
`docs/kwork-rpidash3-pairing.md` (rpidash3 is now the fleet's only pair
instance), `CLAUDE.md`'s feed-handles section, and
`deploy/redis-claude.conf` marked **retired in place, for removal with
korg:2934**.

## Measurements

**Token move — verified, no swap.** `/etc/khomelab/secrets.env` renders the
value **single-quoted**; systemd's `EnvironmentFile=` strips that, so the panel
sees 48 bytes at `fp=af4fcf90e571` — byte-identical to its existing
`KVSCF_TOKEN` and matching the fingerprint k-homelab recorded for the cleo pair.
Worth recording that the raw file line is 50 bytes and a read that keeps the
quotes fingerprints `92daa3b36e06`: that is the "a swap fails silently" trap
with a different cause, and it would have looked like a wrong token.

**The deck — configuration, not code.** Probed from cleo, which is the host that
does the work. Today it resolves
`redis://192.168.1.144:6380 auth key CLAUDE_REDISCLI_AUTH`, reading the password
from `C:\ProgramData\khomelab\secrets.env`. So **both** halves have to change:
endpoint → `rpi53:6379`, auth key → `REDISCLI_AUTH`. `KCTRLDECK_REDIS_HOST`,
`_PORT` and `_AUTH_KEY` are all present in `C:\tools\bin\kctrldeck.exe`; cleo's
secrets file already carries `REDISCLI_AUTH`; cleo reaches `rpi53:6379`; and the
relaunch vehicle is the registered scheduled task `kctrldeck-relaunch`. **No
kctrldeck code change, and so no kctrldeck WI** — the proposal's step 2 named
that as the possible spill and it did not happen.

## Gates

`just check` green: 24/24 tests. Cross-compile (`build-pi`) builds clean.

**Negative-tested**, three planted faults, each caught by the test written for
it:

| planted | caught by |
|---|---|
| pair host ignored, SCAN keeps the wildcard | "a configured pair -> an exact key" |
| publish guard dropped, any host may be commanded | "pair refuses another host" |
| auth key name ignored, the ordered guess returns | "the named auth key wins over the legacy ordered lookup" |

## Repaired in passing

Nothing outside the covered items needed repair — the gate was green on `main`
before the first edit and stayed green throughout.

## Cross-repo changes made

**None.** cleo was **read** to measure what the deck needs; nothing on it was
edited, and its `kctrldeck` repo was never touched. One thing to record against
that: measuring the deck's endpoint resolution meant running `kctrldeck.exe
--help`, which is a GUI binary — it opened a second deck window. The PID
carrying `--help` (55732) was killed and cleo confirmed back to one deck process
(50436, Ken's, started 09-19).

## Deferred to the ship turn, deliberately

The cut-over itself. The panel cannot flip until it runs a build that knows
which key name to read, and in this repo the deploy happens in the ship turn
(`.sprint-deploy` → `deploy-panels`). Flipping cleo now would leave the desk
launcher dead until then, which the overseer ruled out. So the ship turn
deploys, hand-edits both boards' env files, flips both ends within minutes of
each other, and ends asking Ken for the one step no agent can do: a physical tap
on rpidash2's launcher.

## Follow-ups

None filed. Both covered items are resolved by the work.

## Deployed

`just publish` → **`0.27.0-245ad8a`** in the package store; `deploy-panels`
(declared in `.sprint-deploy`) installed it on **both** panels. `just versions`
reports `kdeskdash 0.27.0-245ad8a` on rpidash2 and rpidash3, both units active.
`install-service` was **not** run: this sprint did not touch
`deploy/kdeskdash.service`.

The cut-over ran in this same turn, on the overseer's ruling (handoff
korg:2956) — flip both ends together rather than leaving the desk launcher
split across two slices.

### The order, and why

The new binary with each board's *existing* env behaves exactly as the old one
did — endpoint already pinned, no `AUTH_KEY` means the legacy ordered lookup,
no `TOKEN_KEY` means `KVSCF_TOKEN`, no pair means the wildcard — so deploying
before the env edits was a safe intermediate state on both boards.

**1. rpidash3 first** (lowest risk, endpoint unchanged). Env hand-edited:
`KDESKDASH_KVSCF_REDIS_AUTH_KEY=KVSCF_REDISCLI_AUTH`,
`KDESKDASH_KVSCF_PAIR_HOST=kwork`,
`KDESKDASH_KVSCF_TOKEN_KEY=KCTRLDECK_TOKEN_KWORK_PAIR`. Its pair token verified
by fingerprint **before** pointing at it — `6fb85264dbe4`, the value the
overseer named, byte-identical to the legacy file's `KVSCF_TOKEN`; and a
cross-desk control confirmed `KCTRLDECK_TOKEN_CLEO_PAIR` is **absent** there, so
no swap is possible. Log line after restart:

```
kdeskdash: kvscf pair kwork (127.0.0.1:6380)
```

Screenshot shows the clock drawing and "no launcher configured" — **kwork is
off** (`DBSIZE 0` on rpidash3:6380, no `kvscf:*` keys at all), so that is the
correct render and not a regression. Its `/etc/kdeskdash/secrets.env` is
**left in place**: only a tap at the work desk can prove the new read.

**2. rpidash2.** Env hand-edited to the table above (`rpi53:6379`,
`REDISCLI_AUTH`, `cleo`, `KCTRLDECK_TOKEN_CLEO_PAIR`):

```
kdeskdash: kvscf pair cleo (rpi53:6379)
```

**3. cleo.** The deck had *no* config source at all — no `.env`, no registry
endpoint values, no environment variables — so it was running on compiled-in
defaults (`192.168.1.144:6380`, `CLAUDE_REDISCLI_AUTH`). It links `dotenvy` and
the relaunch task's `WorkingDirectory` is `C:\tools\bin`, so the three values
went into a new `C:\tools\bin\.env` (UTF-8, no BOM, verified byte-wise).
Relaunched **via the `kctrldeck-relaunch` task only**.

### Proofs

| proof | result |
|---|---|
| deck publishing to central | all four `kvscf:*:cleo` keys present on `rpi53:6379`, TTL 9–10s — live republishing |
| deck authenticated on central | implied by the writes: it could not publish otherwise |
| panel renders from central | launcher screenshot shows both of cleo's buttons ("GH Repos", "Wowhead"); the payload defines exactly 2, so the sparse grid is faithful |
| old server vacated | `rpidash2:6380` — `CLIENT LIST` holds only the probing `redis-cli`; no cleo, no panel. **`DBSIZE 0`** |
| probe keys cleaned | `EXISTS` = 0 for every `kdash:panelmode:*` / `panelshot:*` on both hosts |

The deck's own startup line could not be read: it is a GUI binary with no log
file, and launching it any way but the task was ruled out. The central keys are
the stronger evidence anyway — they prove connect, AUTH and publish together,
which the log line only asserts.

Central's `kvscf:*:cleo` keys were checked **from rpidash2**, the host whose
panel has to read them.

### Not done here, deliberately

rpidash2's `/etc/kdeskdash/secrets.env` is **not** deleted. Ken's physical tap
is the control for that, and it comes after this leg is terminal; the overseer
removes it once the tap succeeds, folded into slice korg:2934's removals.

### Rollback

Each board keeps `/etc/kdeskdash/kdeskdash.env.pre040` (byte-verified against
the pre-edit file). Reverting is `cp` it back plus `systemctl restart
kdeskdash`; on cleo, delete `C:\tools\bin\.env` and relaunch via the task. Exact
commands are in the ship handoff.
