# The kwork ↔ rpidash3 pairing

How the work-desk panel is wired to the work machine, and why it is wired that
way rather than the obvious way. This is the one pairing in the fleet where the
publishing machine is **not part of the homelab**: `kwork` is Ken's day-job
machine, deliberately off the tailnet, and every decision below follows from
keeping it that way.

The home-desk pairing used to be the simple case for comparison — `cleo`
published to an instance on rpidash2 and rpidash2 read it over loopback. **It is
no longer a comparison at all.** Sprint 040 folded that pair onto the central
Redis on rpi53, so `rpidash3:6380` is now the fleet's **only** pair instance and
everything below describes the only one of its kind.

What did *not* change, and is the reason this document still stands on its own:
the kwork pair stays here, on its own server, with its own password (kdashdata
CD-8 as amended). kwork is off the tailnet and not part of the homelab, so it
cannot reach central and must not be made to — the whole point of the split. The
panel side reflects that: rpidash3 pins `127.0.0.1:6380`, names its own password
key (`KVSCF_REDISCLI_AUTH`) and its own pairing-token key
(`KCTRLDECK_TOKEN_KWORK_PAIR`), and never falls through to central, which would
put the work desk's launcher on the home feed. Sprint 040 removed the
claude-feed fallback that made such a fall-through possible at all.

(rpidash2's old instance was the claude feed's home too, until sprint 031 moved the feed
to the central Redis and the CD-7 close-out retired the old keys. It served
`kvscf:*` alone after that, and nothing after the fold: it is left running but
unused until the k-homelab cleanup slice retires it. Nothing below changes: the kvscf side was always the reason
for the split, and pinning it is now enforced by `config.c` rather than by
remembering.)

## Topology

```
kwork (Windows, MS-managed, LAN only)          rpidash3 (Pi 4, work desk)
┌───────────────────────────────┐              ┌──────────────────────────────┐
│ kvscf (full build)            │              │ redis-kvscf  :6380           │
│   publishes kvscf:launcher:…  │──── LAN ────▶│   bind 127.0.0.1 + 192.168.… │
│             kvscf:instances:… │   AUTH ✓     │   requirepass ✓              │
│             kvscf:edge:…      │◀─────────────│   ephemeral, 32mb, no save    │
│             kvscf:apps:…      │              └──────────────┬───────────────┘
│   subscribes kvscf:focus:kwork│                             │ loopback
└───────────────────────────────┘              ┌──────────────▼───────────────┐
                                               │ kdeskdash                    │
                                               │   Launcher + Remote modes    │
                                               ├──────────────────────────────┤
                                               │ redis-server :6379           │
                                               │   127.0.0.1 only, no auth    │
                                               │   control / last-mode        │
                                               └──────────────────────────────┘
```

Two Redis instances on rpidash3, which is the same split rpidash2 already runs
and for the same reason: **only the feed instance is ever reachable from the
network**, so mode control, last-mode persistence, the GoL settings injection
and the screenshot trigger stay on a loopback-only 6379 that no other machine
can talk to. WI #1137 originally had the kvscf feed riding 6379 directly; that
would have meant binding the control instance to the LAN, which is precisely
what rpidash2's split exists to avoid.

### Why a second kvscf instance at all

kwork could have published to rpidash2:6380 alongside cleo. Separate won, and
the deciding argument is a code one: `kvscf_redis_init` takes exactly **one**
token, so one shared endpoint would force a per-host token map into the C side
— real work, in exchange for putting work window titles on the home dev-desk
panel. Separate is less code *and* better isolation, and a work-side network
change cannot break the home dashboard. `KDESKDASH_KVSCF_REDIS_*` (WI #664)
exists for exactly this case.

### Why this instance required a password when rpidash2's did not

*Written 2026-08-09, when rpidash2's 6380 instance was open on the trusted home
LAN. That is no longer so: sprint 035 (korg WI 2216) gave rpidash2's instance
the same `requirepass` and loopback-plus-LAN bind, after kmon's nightly reported
it answering unauthenticated across the tailnet — and sprint 040 retired that
instance's last consumer, leaving this the only pair instance in the fleet. The
asymmetry below is history; the three reasons still explain why rpidash3 went
first, and the bring-up for rpidash2's instance is in
`deploy/hosts/README.md`.*

This one was different in three ways:

1. **rpidash3 is dual-homed** (`eth0` + `tailscale0`) and kwork cannot join the
   tailnet, so the LAN listener is mandatory and sits outside the tailnet ACLs
   that cover every other homelab hop.
2. **The data is not equivalent.** The full kvscf build publishes all four
   feeds, so `kvscf:edge:kwork` and `kvscf:instances:kwork` carry work Edge
   window titles and VS Code workspace names. (The launcher feed itself is clean
   by contract — labels, colours, geometry, no URLs.) Those three extra feeds
   are wanted: they are what Remote mode on this panel shows. But they should
   not sit on an unauthenticated LAN service.
3. It costs nothing now. kvscf gained publisher-side AUTH in its sprint 018
   (`KVSCF_REDIS_PASSWORD`, PR #20); kdeskdash's client has taken an `auth`
   argument since Remote mode shipped.

### Two secrets, two layers, two different failure modes

Keep these straight — they are the first thing to check when the panel looks
dead, and they fail in opposite ways.

| | Checked by | Wrong or missing looks like |
|---|---|---|
| `requirepass` / `KVSCF_REDISCLI_AUTH` / `KVSCF_REDIS_PASSWORD` | Redis, at connect | an **unreachable endpoint** — empty Remote list, greyed Launcher grid, `kvscf offline` |
| `KVSCF_TOKEN` (both ends) | kvscf, per command | **nothing at all** — feeds render fine, taps silently do nothing |

## Bring-up

### 1. The Redis instance on rpidash3

The conf and unit are committed; the host-local file carrying the LAN bind
address and the password is not, and Redis **refuses to start without it**. That
is deliberate: "came up without the local file" must never quietly mean "came up
LAN-bound and open".

```sh
# from a clone, e.g. on kai
scp deploy/redis-kvscf.conf   ken@rpidash3:/tmp/
scp deploy/redis-kvscf.service ken@rpidash3:/tmp/

ssh ken@rpidash3
sudo install -m644 /tmp/redis-kvscf.conf    /etc/redis/redis-kvscf.conf
sudo install -m644 /tmp/redis-kvscf.service /etc/systemd/system/redis-kvscf.service

# The host-local half: this board's LAN address + the password. Readable by the
# redis user only. Generate the password somewhere it will not land in a shell
# history or a session transcript.
PW=$(openssl rand -hex 24)
sudo tee /etc/redis/redis-kvscf-local.conf > /dev/null <<EOF
bind 127.0.0.1 -::1 $(hostname -I | awk '{print $1}')
requirepass $PW
EOF
sudo chown root:redis /etc/redis/redis-kvscf-local.conf
sudo chmod 640 /etc/redis/redis-kvscf-local.conf

sudo systemctl daemon-reload
sudo systemctl enable --now redis-kvscf
redis-cli -p 6380 -a "$PW" --no-auth-warning ping     # -> PONG
redis-cli -p 6380 ping                                # -> NOAUTH ... (this is the point)
```

`hostname -I` puts the LAN address first and the tailnet address second on this
board; the `awk` takes the first. Confirm the result before trusting it — a
`bind` naming an address the interface does not have keeps Redis from starting.

### 2. kdeskdash's side of it

`deploy/hosts/rpidash3.env` already points the panel at `127.0.0.1:6380` and
lists `launcher` first in its ops section. The two secrets now come from two
different places (sprint 037) — see
[deploy/hosts/README.md](../deploy/hosts/README.md):

- **The Redis password** is not installed by hand any more. Register it as this
  board's store entry (`redis-kvscf-auth-rpidash3`) in k-homelab and let the
  `khomelab-secrets` recipe render it as `KVSCF_REDISCLI_AUTH` into
  `/etc/khomelab/secrets.env`, which the unit reads. One copy per host,
  regenerated rather than maintained.
- **The token** is still hand-installed, because it has no store entry and who
  issues it is an open question (korg WI 2479).

```sh
sudo tee -a /etc/kdeskdash/secrets.env > /dev/null <<EOF
KVSCF_TOKEN=kvscf-<kwork's own 64hex>
EOF
sudo systemctl restart kdeskdash
```

A device whose `/etc/kdeskdash/kdeskdash.env` predates this sprint will not pick
up the new endpoint or the Launcher: `install-service` never overwrites a
device's env file. Remove it first, then re-run `just install-service rpidash3`.

### 3. kvscf on kwork

kwork has been running **`kvscf-local`** — the feature-gated build with the
entire comms module compiled out, which was the correct choice on that machine
until now. It cannot publish anything. Install the **full `kvscf` build**; they
are separate binaries with different names, and the failure mode of getting this
wrong is quiet (windows enumerate fine locally, the panel just stays empty
forever).

**The five settings do not share one source, and assuming they do breaks the
pairing silently.** Only the two *secrets* read the registry; the other three are
`std::env::var` only, and a registry value for them is ignored whatever its type
(`REG_SZ` or `REG_DWORD` — neither is read). Check `Config::load` in kvscf's
`crates/kvscf-app/src/remote.rs` rather than trusting this table if it ever
looks wrong.

| Value | Where it must go | Setting | Notes |
|---|---|---|---|
| `KVSCF_TOKEN` | **registry**, then env | `kvscf-<64hex>` | **Mandatory** — no token, no channel. kwork's own, distinct from cleo's. |
| `KVSCF_REDIS_PASSWORD` | **registry**, then env | the `requirepass` from step 1 | Optional in kvscf: absent means *no AUTH*, not *no channel*. Required here. |
| `KVSCF_REDIS_HOST` | **env / `.env` only** | rpidash3's LAN address | Not the tailnet one — kwork is not on the tailnet. |
| `KVSCF_REDIS_PORT` | **env / `.env` only** | `6380` | The feed instance, not the control one. |
| `KVSCF_HOST_NAME` | **env / `.env` only** | `kwork` | Set it explicitly rather than letting it derive from the computer name, so the key is predictable. |

Put the three env-only values in a `.env` **beside `kvscf.exe`** — `Config::load`
reads `dotenvy::from_path(<exe dir>/.env)`, and its own comment says that file
exists "for host/port overrides". A user-level `setx` works equally well.

```ini
KVSCF_REDIS_HOST=192.168.1.73
KVSCF_REDIS_PORT=6380
KVSCF_HOST_NAME=kwork
```

Leave the token and password in `HKCU\Software\kenhia\kvscf`: it takes
precedence, and a pinned launch from `C:\tools\bin` has no *cwd* `.env` to fall
back to.

**`HKCU`, not `HKLM` — check which hive `regedit` opened on.** kvscf reads
`HKEY_CURRENT_USER` only. Values under `HKLM\Software\kenhia\kvscf` are invisible
to it, so the token resolves to `None`, `Config::load()` returns `None`, and the
remote channel **never starts at all** — no threads, no socket, and (release
build, see below) not one word of output. That was the first rollout's actual
failure. `HKLM\Software` is also readable by every local user by default, so a
secret parked there is exposed machine-wide and wants rotating, not just moving.

Startup line to look for — **but a release build cannot print it.**
`crates/kvscf/src/main.rs` sets `windows_subsystem = "windows"` for
`not(debug_assertions)`, so there is no console and `eprintln!` goes nowhere.
The publisher also swallows connection errors (`Err(_) => sleep; continue`), so
a release build is *completely silent* about every failure in this section. To
see anything, relaunch with inherited redirected handles:

```powershell
Start-Process -FilePath "<exe>" -RedirectStandardError "$env:TEMP\kvscf-err.txt"
```

```
kvscf: remote channel up — redis://<rpidash3-lan>:6380 auth=true (publish …, focus …)
```

Note it prints immediately after the threads spawn, *before and independent of*
any connection — it means "a token was found", not "it is working". Read
**both halves** of it. `auth=` is printed deliberately, because an endpoint
with a `requirepass` that kvscf has no password for fails as an ordinary
reconnect loop, indistinguishable from an unreachable host. The *address* is
worth the same attention: if the three env-only values never landed, kvscf falls
back to `DEFAULT_HOST`/`DEFAULT_PORT`, which are **rpidash2's** `192.168.1.144`
and `6380` — so the line reads `redis://192.168.1.144:6380` and kwork is aiming
at the wrong board entirely. That was the first rollout's failure: the registry
held all five, kvscf read two of them, and the password it *did* read was then
offered to rpidash2's then-passwordless instance, which rejected AUTH outright.
Both feeds dead on rpidash3, nothing in the log but a reconnect loop. (Since
sprint 035 rpidash2's instance has a password of its own, so a kwork on the
defaults is refused there for the opposite reason — the wrong password rather
than an unwanted one — and can no longer publish work titles onto the home
panel's instance. Same symptom on rpidash3 either way.)

## Verification

Do all four. The first three can each pass while the pairing is still broken.

1. **Transport** — on rpidash3: `redis-cli -p 6380 -a "$PW" --no-auth-warning
   keys 'kvscf:*'` lists `kvscf:launcher:kwork` and friends. If it is empty,
   kvscf on kwork is not publishing: wrong build, missing token, or wrong
   endpoint. Check rpidash2's `6380` too (`redis-cli -p 6380 --scan --pattern
   'kvscf:*'` — with rpidash2's own password in `REDISCLI_AUTH` since sprint
   035) — a `kvscf:*:kwork` key sitting on *that*
   board means the env-only settings never landed and kwork is talking to the
   compiled-in default.
2. **The panel reads it** — the Launcher grid draws kwork's buttons, and Remote
   shows kwork's windows rather than cleo's.
3. **The token round-trips** — tap a launcher button and confirm the URL opens
   **in the Edge window that button prefers** on kwork. This is the only check
   that exercises `KVSCF_TOKEN`, because a wrong token produces no error on
   either side: the feeds keep working and only the command is dropped.
4. **The control instance is still private** — from another machine:
   `redis-cli -h rpidash3 -p 6379 ping` must fail to connect, while
   `redis-cli -h rpidash3 -p 6380 ping` must answer `NOAUTH`. One says the split
   held; the other says the password is on.

## Rollback

Nothing here is entangled with the binary, so the panel and the pairing roll
back separately.

- **The pairing**: `sudo systemctl disable --now redis-kvscf` on rpidash3 and
  stop kvscf on kwork. The panel keeps running; Launcher and Remote go to their
  offline states, which is a designed path, not a crash.
- **Just the mode**: drop `launcher` from `KDESKDASH_MODES` in
  `/etc/kdeskdash/kdeskdash.env` and restart. The mode is inert when not named.
- **The binary**: `just deploy rpidash3 <older version>` — naming an older
  version *is* the rollback. See [deploying.md](deploying.md).
