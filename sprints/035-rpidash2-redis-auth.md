# 035 — rpidash2's kvscf-feed Redis: `requirepass`, LAN-only bind, both consumers migrated

korg: proposal 2231 / work item 2216. Slice 5 of the backlog-drain program
(korg:2233), run as an overseen karc leg on kai. Landed 2026-09-10 23:37 PDT,
well inside the 04:01 nightly window the overseer asked slices 5 and 6 to share.

**Goal:** `redis-claude.service` on rpidash2 answered an unauthenticated `PING`
and `DBSIZE` from kubs0 across the tailnet — item 1 of kmon's nightly plan,
recorded as an open `finding:` in k-homelab's `manifests/rpidash2.yml` (WI 2120).
Decide what still uses the instance, then either bind it to loopback or put a
password on it and migrate every consumer in the same change.

## Premise check

| claim (WI 2216, written 2026-09-10) | verdict |
|---|---|
| the unit documents itself as kdeskdash's | holds — `Documentation=https://github.com/kenhia/kdeskdash` |
| `bind 0.0.0.0:6380`, no `requirepass`, `protected-mode no` | holds, measured live on the box |
| unauthenticated `PING` + `DBSIZE` answer from kubs0; four keys | holds — `PONG`, `4`, re-run from kubs0 before touching anything |
| the `claude:*` family moved to rpi53:6379 on 09-01 and was deleted here | holds — the four keys are all `kvscf:*:cleo`; both panels' env files and `khlenv/store.yml` name rpi53 |
| "what still uses this instance at all?" | **answered**: cleo's kvscf, over the LAN — see below |

## The decision: path 2, with the tailnet listener removed too

The proposal preferred the loopback/stop outcome "if the evidence allows it".
It did not. `CLIENT LIST` on the instance — 29 days of uptime — showed **cleo's
kvscf holding two live connections from 192.168.1.77** (redis-rs 0.27.6): a
`set` publisher for `kvscf:{instances,edge,apps,launcher}:cleo` (10 s TTL,
refreshed every second) and a `subscribe` on `kvscf:focus:cleo`. That is the
dev-desk Launcher/Remote pairing, and it has to cross the LAN. Loopback would
have taken the pairing down; stopping the unit, the same.

So the instance keeps a LAN listener, gets a `requirepass`, and loses the one
listener nothing needed — the tailnet one. This is exactly the shape
`deploy/redis-kvscf.conf` gave rpidash3 in sprint 026, down to the mechanism:
the committed conf binds loopback only and `include`s a hand-installed
`/etc/redis/redis-claude-local.conf` carrying the LAN `bind` and the
`requirepass`, and **Redis fails to start without that file**. "Came up without
the local file" can never again mean "came up LAN-bound and open".

Final listener, the line k-homelab's slice 6 writes into the manifest:
`192.168.1.144:6380`, `127.0.0.1:6380` and `[::1]:6380` — the LAN address, NOT
the tailnet — `auth: requirepass`. The `config:` path is unchanged.

## Consumers — enumerated before the server changed

| consumer | verdict | what was done |
|---|---|---|
| cleo's kvscf (publisher + subscriber, LAN) | **live consumer** | `KVSCF_REDIS_PASSWORD` written to `HKCU\Software\kenhia\kvscf` — the registry-first seam kvscf's sprint 018 built for rpidash3; kvscf relaunched in the console session; re-authenticated within 10 s |
| rpidash2's kdeskdash (Launcher + Remote, loopback) | **live consumer** | `KDESKDASH_KVSCF_REDISCLI_AUTH` appended to `/etc/kdeskdash/secrets.env`; unit restarted; authenticated loopback `get` connection observed with the panel switched to Launcher |
| rpidash3's kdeskdash | not a consumer | pins `127.0.0.1:6380` = its own `redis-kvscf`; `ss` on the box shows no connection to 192.168.1.144 |
| kwork's kvscf | not a consumer | publishes to rpidash3. A kwork left on kvscf's compiled-in default (`192.168.1.144:6380` — rpidash2) is now refused with the wrong password instead of publishing work titles onto the home panel's instance |
| `claude-pub.sh` / kdash-pub (kai, kubs0, cleo, komarchy) | not a consumer | the interim leg was retired by kdashdata 005; the `KDASH_CLAUDE_REDIS` stem has named rpi53:6379 since 09-01 |
| kstudiodash, kpidash, kmon | not consumers | kstudiodash reads rpi53 through libkdash; kmon *probes* it, which is the finding |

A fleet-wide kaed search for `6380` and `192.168.1.144` across kai:src,
kubs0:src, kubs0:k-homelab and the kubsdb roots found nothing beyond docs,
sprint records and kdashdata's endpoint-parsing test fixtures.

## What shipped

- **[deploy/redis-claude.conf](../deploy/redis-claude.conf)** — rewritten:
  `bind 127.0.0.1 -::1`, `protected-mode yes`, and `include
  /etc/redis/redis-claude-local.conf` at the end. Header explains why the
  file keeps the `claude` name (the manifest, the unit and every runbook use it)
  and what it serves now.
- **[deploy/hosts/README.md](../deploy/hosts/README.md)** — new section
  "rpidash2's kvscf-feed instance": bring-up, the consumer table with each
  copy's location and restart step, verification from kubs0, rollback. The
  secrets table and the "each device gets the secrets for its own kvscf"
  paragraph no longer call rpidash2's instance open.
- **[deploy/hosts/rpidash2.env](../deploy/hosts/rpidash2.env)** — comment on
  the kvscf block: AUTH required, the secret's name and where it lives.
- `README.md`, `CLAUDE.md`, `deploy/kdeskdash.env.example`,
  `docs/kwork-rpidash3-pairing.md`, `src/main.c` (comment only),
  `.claude/skills/deploy-panels/SKILL.md` — every sentence that said rpidash2's
  6380 was open, or that kvscf's Claude-feed fallback still pointed at the same
  instance, corrected. The pairing doc's "why this instance requires a password
  when rpidash2's does not" section is kept as history with a dated note.

No C logic changed. `just check`: 21/21.

## What changed on hosts (not in the repo)

**rpidash2** (all with `.pre035` backups beside the originals):
`/etc/redis/redis-claude.conf` replaced with the committed file (md5 verified
both ends); `/etc/redis/redis-claude-local.conf` created, root:redis 0640, two
lines; `/etc/kdeskdash/secrets.env` gained `KDESKDASH_KVSCF_REDISCLI_AUTH`.
`redis-claude` then `kdeskdash` restarted, 06:36:40–06:36:42 UTC.

**cleo:** `KVSCF_REDIS_PASSWORD` (REG_SZ) added to `HKCU\Software\kenhia\kvscf`,
and a scheduled task **`kvscf-relaunch`** registered — no triggers, interactive
logon, `ExecutionTimeLimit PT0S`, action `C:\tools\bin\kvscf.exe`. It exists
because kvscf is a session-1 GUI app that enumerates windows, and a
`Start-Process` from an ssh session lands in session 0, invisible and blind. The
task is the restart step of this credential's rotation and is named as such in
the deploy README and krot WI 2302, so it stays.

The password was generated in-shell on kai (`openssl rand -hex 24`) and piped
over stdin to both hosts inside one command; it appears nowhere in this
sprint's transcript, korg or klams. Only its fingerprint does.

## Verification

| check | from | result |
|---|---|---|
| `redis-cli -h rpidash2 -p 6380 PING` | **kubs0** (the host kmon probes from) | `NOAUTH Authentication required` |
| `redis-cli -h rpidash2 -p 6380 DBSIZE` | kubs0 | NOAUTH |
| `redis-cli -h 100.124.180.71 -p 6380 PING` (tailnet address) | kubs0, kai | connection refused |
| `redis-cli -h rpidash2 -p 6379 PING` (control instance) | kubs0 | connection refused, as before |
| `redis-cli -h rpidash3 -p 6380 PING` (sibling, untouched) | kubs0 | NOAUTH, as before |
| `REDISCLI_AUTH=… redis-cli -p 6380 PING` | rpidash2 | `PONG`, on loopback and on 192.168.1.144 |
| `--scan` after kvscf's relaunch | rpidash2 | four `kvscf:*:cleo` keys, TTL 10 |
| `CLIENT LIST` | rpidash2 | cleo's `set` + `subscribe` from 192.168.1.77, authenticated; a `127.0.0.1` `get` while the panel is in Launcher |
| `kddss` with the panel switched to `launcher`, then `foreground` | kai → rpidash2 | both modes render cleo's feed (`.scratch/035/*.png`); mode restored to `claude` |

Both kai and kubs0 resolve `rpidash2` to the LAN address, so "reachable across
the tailnet" in the finding was literally true only via the `0.0.0.0` bind on
`100.124.180.71`; the probe kmon runs from kubs0 goes over the LAN and now meets
`NOAUTH`, which is the right answer for a listener that exists to be reached
from another machine.

## Decisions worth keeping

- **Enumerate from the server, not just the configs.** `CLIENT LIST` on a
  29-day-old instance is the one source that names *every* live client with its
  address and library. It found the consumer the proposal's "first find what
  still uses it" was asking about in one command; the config sweep then
  confirmed there was no second one.
- **Consumer copies first, server second, restarts last.** Both copies are
  inert until their process restarts, so writing them before the server flip
  keeps the outage to the two restarts (~20 s of a dimmed Launcher on the dev
  desk at 23:36).
- **A pinned taskbar launch has no restart path from ssh.** Recorded in the
  README and in klams: the answer is an interactive-logon scheduled task, not
  `Start-Process`.
- **krot registration from a non-cleo host is a work item, by the skill's own
  rule.** `krot-register` forbids editing `D:\ClaudeWorks\krot` over ssh; from
  kai it prescribes a `registration`-tagged krot WI with the full facts. That is
  **krot WI 2302**, which also names rpidash3's `redis-kvscf` password —
  k-homelab already records it as "registered nowhere" — as the sibling to
  register in the same pass.

## Follow-ups

- **krot WI 2302** — register both instances' `requirepass` (the overseer may
  fold it into korg:2230, or Ken runs it from cleo).
- **k-homelab slice 6 (korg:2227)** — clear the `finding:` in
  `manifests/rpidash2.yml` with the listener line above; consider mirroring
  rpidash3's `expected_state` entry ("two instances, only one authenticated,
  and that is correct"); cleo's §7 register may want the task and registry
  value.
- `src/config.c`'s Claude-feed fallback for `KDESKDASH_KVSCF_REDIS_*` is now
  dead weight (unset follows the Claude feed to rpi53, where there is no kvscf).
  CD-8 already says pin explicitly and both panels do; removing the fallback is
  a small tweak for a later sprint, not this one.
- `.github/copilot-instructions.md` still describes the pre-031 topology (feed
  on 6380, same instance as kvscf). Stale before this sprint; left alone.
- `scripts/kddss` can `cat` the BMP while the device is still writing it — one
  of two shots came back truncated and a retry succeeded. A size-stable check,
  or a write-then-rename on the device side, would close it.
