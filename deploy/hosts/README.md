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

## Secrets

Four variables must never reach the repo, so they live in a second file the
unit reads optionally, hand-installed once per device:

| Var | What it unlocks | Missing ⇒ |
|---|---|---|
| `KVSCF_TOKEN` | Remote and Launcher taps — the *application* gate on focus/launch/press commands | list and grid still show, taps do nothing (Remote says "view only") |
| `KDESKDASH_TELEMETRY_REDISCLI_AUTH` | the kpidash telemetry Redis on rpi53, which requires AUTH | Dev mode activates but shows no host data |
| `KDESKDASH_CLAUDE_REDISCLI_AUTH` | the Claude feed, on that **same** rpi53 Redis since sprint 031 — same password, separate handle, so it must be set separately | Claude mode activates and shows nothing: no sessions, no usage gauges |
| `KDESKDASH_KVSCF_REDISCLI_AUTH` | the kvscf Redis — the *transport* gate. **Both panels**: each board's own 6380 instance requires AUTH (rpidash3's `redis-kvscf` since sprint 026, rpidash2's `redis-claude` since sprint 035) | Remote and Launcher never connect at all: empty list, cached-and-dimmed grid, `kvscf offline` |

```bash
sudo install -d -m755 /etc/kdeskdash
sudo install -m600 /dev/null /etc/kdeskdash/secrets.env
sudo tee -a /etc/kdeskdash/secrets.env > /dev/null <<'EOF'
KVSCF_TOKEN=kvscf-<64hex>
KDESKDASH_TELEMETRY_REDISCLI_AUTH=<rpi53 redis password>
KDESKDASH_CLAUDE_REDISCLI_AUTH=<the same rpi53 redis password>
KDESKDASH_KVSCF_REDISCLI_AUTH=<this board's kvscf redis password — the requirepass in its /etc/redis/redis-*-local.conf>
EOF
sudo systemctl restart kdeskdash
```

The unit lists this file after the config file, so a value set here also
overrides the committed one. Every entry uses systemd's leading `-`, so a device
without the file starts fine — it just runs with those features degraded as
above.

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
| rpidash2's kdeskdash (Launcher + Remote, over loopback) | `/etc/kdeskdash/secrets.env` → `KDESKDASH_KVSCF_REDISCLI_AUTH` | `sudo systemctl restart kdeskdash` |

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
printf 'KDESKDASH_KVSCF_REDISCLI_AUTH=%s\n' "$PW" | sudo tee -a /etc/kdeskdash/secrets.env > /dev/null

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
`/etc/redis/redis-claude.conf.pre035` over the conf, remove the
`KDESKDASH_KVSCF_REDISCLI_AUTH` line from `secrets.env`, clear
`KVSCF_REDIS_PASSWORD` from cleo's registry key, restart all three. That is
the exposure back, so it is a rollback for a broken pairing, not a standing
option.

**The telemetry and Claude passwords are the same string** — since sprint 031
both handles read rpi53:6379 — and they are still two variables, because they
are two independent connections with independent failure isolation. Setting one
and not the other is the quiet way to get a blank Claude mode on a panel whose
Dev mode is fine.
