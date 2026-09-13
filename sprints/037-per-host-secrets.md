# Sprint 037 — the panel reads the fleet's per-host secrets file

**Proposal:** korg:2430, slice 11 of program korg:2440 (simplify homelab
secrets). **Covers:** WI 2408. Run as karc leg `kdeskdash-614cfe` on kai, under
`overseen-sprint` — the ship is gated on the overseer's clearance.

## Goal

Stop this panel keeping its own copies of fleet passwords. k-homelab renders
`/etc/khomelab/secrets.env` per host from the age store (sprint 059, korg:2426);
the unit reads it, and the two `KDESKDASH_*_REDISCLI_AUTH` variables that were
one secret collapse into the fleet's one name.

## Premise check

Every falsifiable claim in WI 2408 checked live on both panels before starting,
fingerprints only (`sha256[:12]`, via `sudo cat … | tr`, never the redirect):

| claim | verdict |
|---|---|
| both panels hold `KDESKDASH_TELEMETRY_REDISCLI_AUTH` and `KDESKDASH_CLAUDE_REDISCLI_AUTH`, and they are the *same* secret | **holds** — both `76cde5f57955` on both boards, the same value k-homelab records for `rediscli-auth-rpi53` |
| that value is the central rpi53 password now in the per-host file | **holds** — `REDISCLI_AUTH` in `/etc/khomelab/secrets.env` is `76cde5f57955` on both |
| plus `KDESKDASH_KVSCF_REDISCLI_AUTH` for the board's own kvscf Redis | **holds**, and the two boards' values genuinely differ: `daa601b6368a` (rpidash2) / `d3f6d2c72545` (rpidash3) |
| "the install stops writing `secrets.env`" | **drifted, harmlessly** — the install never wrote it. `scripts/deploy.sh` has said "Secrets are NOT installed here" since sprint 018; the file has always been hand-installed. Nothing to remove |
| "rpidash3 is intermittent; deploy when up" | **gone**, as the proposal's own correction says — both panels answered first try |

## The thing that made this sprint

**`REDISCLI_AUTH` was already taken — by the control Redis** (`config.c:37`).

The control handle talks to this board's *own* instance on 6379, which is
loopback-only and passwordless on both panels. The moment the unit gained
`EnvironmentFile=-/etc/khomelab/secrets.env`, that handle would have started
sending the central password to it. Measured on both boards:

```
$ REDISCLI_AUTH=anything redis-cli -p 6379 ping
AUTH failed: ERR AUTH <password> called without any password configured
```

Remote mode control, last-mode persistence, GoL injection and the screenshot
trigger would all have stopped, silently — the panel keeps drawing. It is
sprint 031's failure one variable to the left, and the diff that causes it
(one line in a unit file) does not mention the variable it breaks.

So the control handle moved to `KDESKDASH_CONTROL_REDISCLI_AUTH`, and
`REDISCLI_AUTH` now means what the fleet says it means. Written up as
`docs/solutions/best-practices/a-generic-env-name-is-a-namespace-you-joined.md`.

## The key names, and the decision the proposal left here

| handle | endpoint | key | source |
|---|---|---|---|
| control | this board, `:6379` | `KDESKDASH_CONTROL_REDISCLI_AUTH` | unset — no password |
| telemetry | `rpi53:6379` | `REDISCLI_AUTH` | fleet file |
| claude feed | `rpi53:6379` | `REDISCLI_AUTH` | fleet file |
| service card | `rpi53:6379` | inherits telemetry (same-endpoint rule) | fleet file |
| kvscf | this board, `:6380` | `KVSCF_REDISCLI_AUTH`, then `CLAUDE_REDISCLI_AUTH` | fleet file |

**The proposal's suggested resolution was not available.** Its notes said a
single slot name could map to each panel's own store entry. k-homelab's shipped
recipe forbids exactly that: `bin/check-secrets` **refuses a tree where one key
names different store entries on different hosts**, and its README states the
ruling in as many words — WI 2399 chose "one name per *secret*" over "one name
per *slot*", so "a consumer that runs on both panels reads the name its own host
declares."

So this slice took the proposal's stated alternative: kdeskdash reads the two
published per-host names in order. Exactly one is set on each panel
(`CLAUDE_REDISCLI_AUTH` on rpidash2, `KVSCF_REDISCLI_AUTH` on rpidash3), so the
order is a tie-break that never fires; the slot name goes first because it
describes what the handle is for, and if rpidash2's key is ever renamed the
fallback becomes dead code.

**Consequence: no cross-repo change at all.** The Branch A manifest edit the
proposal pre-authorised would have been refused by k-homelab's own gate.

On rpidash2, note that `CLAUDE_REDISCLI_AUTH` is **not** the Claude feed's
password — it is named for the `redis-claude` *instance*, which has served kvscf
since sprint 031. The feed reads central.

## What shipped

- `src/config.c` / `config.h` — the four key names above, each with the
  reasoning inline.
- `deploy/kdeskdash.service` — reads three env files in precedence order: the
  fleet file, this host's committed config, then `KVSCF_TOKEN`'s file. All
  optional (`-`), so a missing file degrades rather than blocks boot.
  **No `SupplementaryGroups=khomelab`** — the unit runs as root, and systemd
  reads `EnvironmentFile=` as PID 1 before dropping privileges anyway, so it
  buys nothing while making the unit fail to start on a host without the group
  (korg handoff 2493).
- `scripts/unit-lint.sh` + `just unit-lint`, wired into `just check` — four
  static clauses, each driven to exit 1 before being trusted.
- `tests/test_config.c` — seven new cases, including the regression test that
  asserts setting `REDISCLI_AUTH` leaves the control handle *unauthenticated*,
  and both panels' key-name shapes.
- `scripts/deploy.sh` — `install-service` now warns on unit/binary version skew
  (see below).
- Docs: the unit header, both host env files, `kdeskdash.env.example`,
  `deploy/hosts/README.md`, the top-level README's variable table,
  `docs/kwork-rpidash3-pairing.md`, and the new best-practice note.

### The ordering hazard this change creates

**The unit and the binary are not independently deployable.** A kdeskdash older
than 037 reads bare `REDISCLI_AUTH` as its control password, so installing the
new unit on a board still running an old binary is precisely the breakage above.
`just install-service` and `just deploy` are two commands, and running the first
without the second is now dangerous. `install-service` therefore probes the
installed binary and warns when it does not match the unit's artifact version —
a warning, not a refusal, because a fresh device legitimately has no binary yet.

## `KVSCF_TOKEN` stays where it is

WI 2479 (Awaiting Ken) asks whether it becomes a store key and who its
authority is; both panels hold different values and neither is in the store.
Unanswered as of this sprint, so `/etc/kdeskdash/secrets.env` is **not** deleted
— it is that key's only home, and the unit still reads it.

## Verified live, and from where

Everything from **kai**, which is the host that runs `just deploy` and
`just push-dev`, or on the panels themselves. No value printed anywhere.

**Credentials, both panels, each with a wrong-password control** — the control
matters because the old and new values are identical, so a fresh connection
proves nothing on its own:

- central `rpi53:6379` with the fleet file's `REDISCLI_AUTH` → `PONG`;
  with a wrong value → `WRONGPASS`. Both boards.
- each board's own `:6380` with its own fleet key → `PONG`; wrong → `WRONGPASS`.
  rpidash2 via `CLAUDE_REDISCLI_AUTH`, rpidash3 via `KVSCF_REDISCLI_AUTH` —
  both published shapes exercised.

**Full stack on rpidash2** (new unit + new binary installed together, in that
order, so the old binary never started under the new unit):

- `systemctl show -p EnvironmentFiles` → exactly the three files,
  all `ignore_errors=yes`. systemd *parsed* them; neither the repo's bytes nor
  the panel's copy of them can show that.
- the running process carries `REDISCLI_AUTH` (`76cde5f57955`) and
  `CLAUDE_REDISCLI_AUTH` (`daa601b6368a`) and **not**
  `KDESKDASH_CONTROL_REDISCLI_AUTH`. Those two names exist *only* in the fleet
  file on this host, so the environment itself proves the source.
- **control handle, the collision test**: `SET kdeskdash:active_mode palette`
  over the local Redis switched the panel and it wrote the new value back;
  restored to `claude`. Read and write, both directions, no AUTH sent.
- **service card**: `kpidash:services:deskdash:rpiDash2` (the mixed-case
  hostname) fresh within 2 s, `state: ok`, carrying the pushed build's version.
  That is the central password authenticating end-to-end from the new binary.
- kvscf: four `kvscf:*:cleo` keys readable on `:6380`, three clients connected.
- no auth, `NOAUTH`, `WRONGPASS` or error lines in the journal; zero failed units.

**Negative control** — the fleet file moved aside and restored (kstudiodash's
method): both variables vanished from the restarted process, the card's last
write was its own `state: "down"` shutdown record and then stopped advancing.
Restored, md5 identical before and after, card `ok` and 2 s fresh again.

**rpidash3 was not restarted or redeployed.** Its credentials and controls were
probed from the board itself; the code path its key name exercises is covered by
`just check`. It picks the change up on the post-ship deploy.

## Left behind

- **rpidash2 is running `0.27.0-3c93d5f-dirty`**, a `push-dev` build, until the
  post-ship `just publish` + `just deploy`. The version string says so, and
  `just versions` reports it as not-in-the-store by design.
- **No soak.** Nothing in the acceptance is time-gated.
- **No new work items.** Both defects found were repairs by the rule.

## Repaired in passing

- **`scripts/deploy.sh` gained the version-skew warning** above. Not
  pre-existing — this sprint created the hazard, so guarding it is part of the
  change rather than an extra.
- **`docs/kwork-rpidash3-pairing.md` told you to hand-install the Redis
  password** into `secrets.env`, which this sprint falsified. Corrected to the
  store-entry route, with the token's exception kept.
- **`a-fallback-outlives-the-sameness-that-justified-it.md`** quoted a variable
  this sprint renamed; noted as quoted-as-it-stood and cross-linked to the new
  page, since the two failures share a symptom worth recognising.

## For the changeover (korg:2436)

- The three retired names are **still set** in the running process on both
  panels, from the old `/etc/kdeskdash/secrets.env`. They are inert — nothing
  reads them — and deleting those three lines is 2436's, per WI 2408's own
  acceptance. The file itself must survive: `KVSCF_TOKEN` is in it.
- rpidash2 also carries `secrets.env.pre031` and `secrets.env.pre035`, and
  `kdeskdash.env.pre031` / `.pre-sprint018.bak`. Older copies of the same
  passwords, read by nothing.
- **No `secrets_group_members` entry is needed for kdeskdash on either panel.**
  The unit runs as root. The manifests' "each consumer slice adds its own"
  therefore has no work for this one — and a working service would have been no
  evidence either way.

## Deployed

**`0.27.0-7f67c50`** (squash `7f67c50`, PR #44) published to the package store
and installed on **both** panels, 2026-09-12. Run from kai, which is the only
host that can deploy — the Pis are unmanaged and hold no store credentials.

**Unit first, then binary, on each board** — the order this sprint's own hazard
requires, and the reason `install-service` now warns. Both warnings fired for
real on the way through, which is the guard working on its author:

```
deploy.sh: WARNING — this unit is from 0.27.0-7f67c50 but ken@rpidash2 is
           running 'kdeskdash 0.27.0-3c93d5f-dirty'.
deploy.sh: WARNING — this unit is from 0.27.0-7f67c50 but ken@rpidash3 is
           running 'kdeskdash 0.27.0-ece5bc8'.
```

`install-service` does not restart, so the old binary never ran under the new
unit on either board; the deploy that followed seconds later is what restarted
them.

| | rpidash2 | rpidash3 |
|---|---|---|
| `--version` | `kdeskdash 0.27.0-7f67c50` | `kdeskdash 0.27.0-7f67c50` |
| unit | active | active |
| `EnvironmentFiles` | the three, in order | the three, in order |
| `KDESKDASH_CONTROL_REDISCLI_AUTH` | absent — no AUTH to the local instance | absent |
| fleet keys in the process | `REDISCLI_AUTH`, `CLAUDE_REDISCLI_AUTH` | `REDISCLI_AUTH`, `KVSCF_REDISCLI_AUTH` |
| control round-trip | `claude → palette → claude` | `menu → palette → menu` |
| service card on central | `ok`, 2 s fresh, `0.27.0-7f67c50` | `ok`, 6 s fresh, `0.27.0-7f67c50` |
| frame | `deploy-037-rpidash2.png` | `deploy-037-rpidash3.png` |

**Both per-host key shapes are live**, which is the thing only a real deploy
could show: rpidash2 resolves its 6380 password from `CLAUDE_REDISCLI_AUTH` and
rpidash3 from `KVSCF_REDISCLI_AUTH`, from one binary with no per-host build.

**The collision test passed on both boards**, live: a remote mode change written
to each board's own passwordless Redis round-tripped, so the control handle is
connecting and sending no AUTH — with `REDISCLI_AUTH` set in its environment
throughout. That is the failure this sprint existed to prevent, verified on the
released build rather than argued.

rpidash2's frame shows Claude mode rendering live fleet activity, so the central
credential is authenticating end to end and not merely connecting.

Nothing was left pending: both boards were reachable, and rpidash2 is off the
`push-dev` build it was carrying for the pre-ship check.
