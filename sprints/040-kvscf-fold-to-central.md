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
