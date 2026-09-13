# Per-host deploy config

One file per device, committed, **no secrets**. `just install-service <host>`
installs `<host>.env` to `/etc/kdeskdash/kdeskdash.env` — but only if that file
is absent, so hand edits made on the device are never clobbered. To roll out a
change to a host that already has the file, edit it there or remove it first.

`../kdeskdash.env.example` remains the full-surface reference: every variable
the app reads, with its built-in default. These host files carry only what
differs or is worth pinning per device.

Adding a host is one file: `deploy/hosts/<host>.env`, then
`just install-service <host>` + `just deploy <host>`. A host with no file yet
still installs — `scripts/deploy.sh` falls back to the example and says so.

## Passwords: the fleet's per-host file

Since sprint 037 the Redis passwords are **not this repo's to install**. k-homelab
renders one file per host from the age store, and the unit reads it:

```
/etc/khomelab/secrets.env      root:khomelab 0640      KEY='value'
```

Two keys matter to a panel. Both are fleet names — one name per secret,
fleet-wide — so nothing about them is set in a committed host file:

| Key | What it unlocks | Missing ⇒ |
|---|---|---|
| `REDISCLI_AUTH` | the **central** rpi53 Redis: Dev-mode telemetry, the Claude feed, and the service card that puts this panel on the kpidash board. Three connections, one secret | Dev shows no host data, Claude mode shows nothing, and the panel goes missing from the board |
| `KVSCF_REDISCLI_AUTH` *or* `CLAUDE_REDISCLI_AUTH` | this board's **own** 6380 instance — the *transport* gate for Remote and Launcher. Which name, see below | Remote and Launcher never connect: empty list, cached-and-dimmed grid, `kvscf offline` |

### The one place the key name is per-host

The two boards' 6380 instances are genuinely **different services with
different passwords**, so the fleet gives them **different key names** — and
that is a ruling, not an oversight. k-homelab's WI 2399 chose "one key name per
*secret*" over "one key name per *slot*", and `bin/check-secrets` **refuses** a
tree where one key names different store entries on different hosts. So a
single tidy `KVSCF_REDISCLI_AUTH` mapped per host is not available; each panel
declares the name of the secret it actually holds:

| host | instance | key in the fleet file | store entry |
|---|---|---|---|
| rpidash2 | `redis-claude` (the name is historical; it has served kvscf since sprint 031) | `CLAUDE_REDISCLI_AUTH` | `redis-claude-auth-rpidash2` |
| rpidash3 | `redis-kvscf` | `KVSCF_REDISCLI_AUTH` | `redis-kvscf-auth-rpidash3` |

kdeskdash therefore reads the two published names **in order** —
`KVSCF_REDISCLI_AUTH`, then `CLAUDE_REDISCLI_AUTH` — rather than carrying a
per-host build or a config knob. Exactly one is set on each panel, so the order
is a tie-break that never fires in practice; the slot name is first because it
describes what the handle is for. If rpidash2's key is ever renamed to match,
the first lookup starts winning and the second becomes dead code.

On rpidash2, note carefully that `CLAUDE_REDISCLI_AUTH` is **not** the Claude
feed's password. It is named after the *instance*, and the Claude feed has read
central (`REDISCLI_AUTH`) since sprint 031.

### The name that is deliberately not `REDISCLI_AUTH`

The control Redis — this board's own instance on 6379 — reads
**`KDESKDASH_CONTROL_REDISCLI_AUTH`**, and is unset on both panels because that
instance is loopback-only and passwordless.

It must never read the bare name. `REDISCLI_AUTH` is the central password and is
*always* set once the unit reads the fleet file, and a Redis with no password
configured answers `AUTH` with an **error**, not a shrug — so the control handle
would stop connecting the moment the file arrived, taking remote mode control,
last-mode persistence, GoL injection and the screenshot trigger with it, while
the panel carried on drawing. `scripts/unit-lint.sh` fails the build if that
name comes back; `tests/test_config.c` fails if the behaviour does.

## The one hand-installed secret

`KVSCF_TOKEN` is still hand-installed, and is the only thing left in
`/etc/kdeskdash/secrets.env`:

| Var | What it unlocks | Missing ⇒ |
|---|---|---|
| `KVSCF_TOKEN` | Remote and Launcher taps — the *application* gate on focus/launch/press commands | list and grid still show, taps do nothing (Remote says "view only") |

It is not in the age store and not in the fleet file, because the two panels
hold **different values** and who issues it is an open question — korg WI 2479,
awaiting Ken. Until that is answered this file stays, holding this one key.

```bash
sudo install -d -m755 /etc/kdeskdash
sudo install -m600 /dev/null /etc/kdeskdash/secrets.env
printf 'KVSCF_TOKEN=kvscf-<64hex>\n' | sudo tee -a /etc/kdeskdash/secrets.env > /dev/null
sudo systemctl restart kdeskdash
```

The unit reads three files, later winning over earlier: the fleet file, this
host's committed config, then this one. Every entry uses systemd's leading `-`,
so a device missing any of them still starts — degraded as above rather than
dark, which is the only recoverable failure on a panel whose repair story is
SSH.

**No `SupplementaryGroups=khomelab`.** The unit runs as root, and systemd reads
`EnvironmentFile=` as PID 1 before dropping privileges anyway — so the group
grants nothing, while naming a group a host does not have makes the unit *fail
to start*. Measured on systemd 259 (korg handoff 2493); the lint enforces it.

The two kvscf secrets are independent and fail differently, which is worth
keeping straight when a panel looks dead: the **token** is checked by kvscf on
the far end, so a wrong one produces no error anywhere and only shows up as taps
that do nothing; the **password** is checked by Redis, so a wrong one looks like
an unreachable endpoint.

Each device gets the secrets for *its own* kvscf: rpidash2 talks to cleo's over
the second Redis instance on rpidash2 itself (`redis-claude` — the name is
historical — authenticated since sprint 035); rpidash3 talks to kwork's over the
second instance on rpidash3 itself (`redis-kvscf`, authenticated since sprint
026). Each password is per instance and shared with that instance's publisher,
which holds it as `KVSCF_REDIS_PASSWORD` (cleo / kwork). The work pairing is in
[docs/kwork-rpidash3-pairing.md](../../docs/kwork-rpidash3-pairing.md); the dev
desk's instance is below.

## rpidash2's kvscf-feed instance (`redis-claude`)

`deploy/redis-claude.conf` + `deploy/redis-claude.service`, hand-installed (there
is no `just` recipe for the Redis instances — they change once a year). Sprint
035 (korg WI 2216) took it from `bind 0.0.0.0`, no password — which kmon's
nightly reported as an unauthenticated Redis answering across the tailnet — to
the shape rpidash3's instance has had since sprint 026: the committed conf binds
loopback only and `include`s a host-local file that adds the LAN address and the
`requirepass`. Redis **fails to start** without that file, on purpose.

Consumers, both of which hold the password and both of which must restart when
it changes:

| consumer | where the copy lives | restart |
|---|---|---|
| cleo's kvscf (publishes `kvscf:*:cleo`, subscribes `kvscf:focus:cleo`, from the LAN) | `HKCU\Software\kenhia\kvscf` → `KVSCF_REDIS_PASSWORD` (registry first, then env / `.env`; kvscf sprint 018) | relaunch `kvscf.exe` in the console session — from ssh, `Start-ScheduledTask -TaskName kvscf-relaunch` (an interactive-logon task with no triggers, left registered for this purpose) |
| rpidash2's kdeskdash (Launcher + Remote, over loopback) | `/etc/khomelab/secrets.env` → `CLAUDE_REDISCLI_AUTH`, rendered by k-homelab from `redis-claude-auth-rpidash2` | `sudo systemctl restart kdeskdash` |

Nothing else reads it: the `claude:*` publishers and both panels' Claude modes
moved to rpi53:6379 in sprint 031 (kdashdata CD-7), rpidash3 reads its own
instance, and kwork publishes to rpidash3 — a kwork that falls back to kvscf's
compiled-in default (`192.168.1.144:6380`, rpidash2) is now refused here.

```bash
# On rpidash2. The host-local half: this board's LAN address + the password.
# Generate the password somewhere it will not land in a shell history or a
# session transcript; the same value goes into the two consumer copies above.
PW=$(openssl rand -hex 24)
sudo tee /etc/redis/redis-claude-local.conf > /dev/null <<EOF
bind 127.0.0.1 -::1 $(hostname -I | awk '{print $1}')
requirepass $PW
EOF
sudo chown root:redis /etc/redis/redis-claude-local.conf
sudo chmod 640 /etc/redis/redis-claude-local.conf
# The panel's copy is NOT written here any more: register the value as this
# board's store entry in k-homelab (redis-claude-auth-rpidash2) and let the
# recipe render it as CLAUDE_REDISCLI_AUTH into /etc/khomelab/secrets.env.

sudo systemctl restart redis-claude
redis-cli -p 6380 ping                                  # -> NOAUTH ... (this is the point)
REDISCLI_AUTH=$PW redis-cli -p 6380 ping                # -> PONG
redis-cli -h 100.124.180.71 -p 6380 ping                # -> Connection refused (not on the tailnet)
sudo systemctl restart kdeskdash                        # picks up the secret
```

`hostname -I` puts the LAN address first (`192.168.1.144`) and the tailnet
address second on this board; the `awk` takes the first. Then set cleo's copy
and relaunch kvscf; within a second `REDISCLI_AUTH=$PW redis-cli -p 6380 --scan`
shows the four `kvscf:*:cleo` keys again and `CLIENT LIST` shows cleo's two
connections (one `set`, one `subscribe`) from `192.168.1.77`.

Verification from another machine — the acceptance test for WI 2216 was run
from **kubs0**, the host kmon probes from:
`redis-cli -h rpidash2 -p 6380 ping` → `NOAUTH Authentication required` (the
LAN listener is reachable and closed), and against the tailnet address →
connection refused (there is no tailnet listener). The control instance on 6379
is unchanged: loopback-only, no password, refuses off-box connections.

Rollback: `sudo systemctl stop redis-claude`, restore
`/etc/redis/redis-claude.conf.pre035` over the conf, drop `CLAUDE_REDISCLI_AUTH`
from this host's key list in k-homelab's manifest and re-apply, clear
`KVSCF_REDIS_PASSWORD` from cleo's registry key, restart all three. That is
the exposure back, so it is a rollback for a broken pairing, not a standing
option.

**The telemetry and Claude passwords were the same string** — since sprint 031
both handles read rpi53:6379 — and until sprint 037 they were still two
variables, which made "set one and not the other" the quiet way to get a blank
Claude mode on a panel whose Dev mode was fine. They are now one name,
`REDISCLI_AUTH`, so that failure is unreachable. The handles are still two,
because failure isolation was never what the two *variables* bought.
