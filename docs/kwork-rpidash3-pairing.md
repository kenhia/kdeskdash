# The kwork ↔ rpidash3 pairing

How the work-desk panel is wired to the work machine, and why it is wired that
way rather than the obvious way. This is the one pairing in the fleet where the
publishing machine is **not part of the homelab**: `kwork` is Ken's day-job
machine, deliberately off the tailnet, and every decision below follows from
keeping it that way.

The home-desk pairing is the simple case for comparison — `cleo` publishes to
rpidash2's open claude-feed instance and rpidash2 reads it over loopback. Read
this document only for the work desk.

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

### Why this instance requires a password when rpidash2's does not

rpidash2's claude feed is open on the trusted home LAN, and that stays true.
This one is different in three ways:

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
| `requirepass` / `KDESKDASH_KVSCF_REDISCLI_AUTH` / `KVSCF_REDIS_PASSWORD` | Redis, at connect | an **unreachable endpoint** — empty Remote list, greyed Launcher grid, `kvscf offline` |
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
lists `launcher` first in its ops section. The password and token are secrets,
so they go in `secrets.env` by hand — see
[deploy/hosts/README.md](../deploy/hosts/README.md).

```sh
sudo tee -a /etc/kdeskdash/secrets.env > /dev/null <<EOF
KDESKDASH_KVSCF_REDISCLI_AUTH=$PW
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

Config resolves from `HKCU\Software\kenhia\kvscf` first, then env / `.env`.
**Prefer the registry**: a pinned launch from `C:\tools\bin` has no cwd or
exe-dir `.env`, and that failure is silent — the channel simply never comes up.

| Value | Setting | Notes |
|---|---|---|
| `KVSCF_TOKEN` | `kvscf-<64hex>` | **Mandatory** — no token, no channel. kwork's own, distinct from cleo's. |
| `KVSCF_REDIS_PASSWORD` | the `requirepass` from step 1 | Optional in kvscf: absent means *no AUTH*, not *no channel*. Required here. |
| `KVSCF_REDIS_HOST` | rpidash3's LAN address | Not the tailnet one — kwork is not on the tailnet. |
| `KVSCF_REDIS_PORT` | `6380` | The feed instance, not the control one. |
| `KVSCF_HOST_NAME` | `kwork` | Set it explicitly rather than letting it derive from the computer name, so the key is predictable. |

Startup line to look for:

```
kvscf: remote channel up — redis://<rpidash3-lan>:6380 auth=true (publish …, focus …)
```

`auth=` is printed deliberately. An endpoint with a `requirepass` that kvscf has
no password for fails as an ordinary reconnect loop — indistinguishable from an
unreachable host unless you can see whether AUTH was even attempted.

## Verification

Do all four. The first three can each pass while the pairing is still broken.

1. **Transport** — on rpidash3: `redis-cli -p 6380 -a "$PW" --no-auth-warning
   keys 'kvscf:*'` lists `kvscf:launcher:kwork` and friends. If it is empty,
   kvscf on kwork is not publishing: wrong build, missing token, or wrong
   endpoint.
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
