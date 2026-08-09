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

Three variables must never reach the repo, so they live in a second file the
unit reads optionally, hand-installed once per device:

| Var | What it unlocks | Missing ⇒ |
|---|---|---|
| `KVSCF_TOKEN` | Remote and Launcher taps — the *application* gate on focus/launch/press commands | list and grid still show, taps do nothing (Remote says "view only") |
| `KDESKDASH_TELEMETRY_REDISCLI_AUTH` | the kpidash telemetry Redis on rpi53, which requires AUTH | Dev mode activates but shows no host data |
| `KDESKDASH_KVSCF_REDISCLI_AUTH` | the kvscf Redis, when that endpoint requires AUTH — the *transport* gate. rpidash3 only; rpidash2's kvscf shares the open claude-feed instance | Remote and Launcher never connect at all: empty list, cached-and-dimmed grid, `kvscf offline` |

```bash
sudo install -d -m755 /etc/kdeskdash
sudo install -m600 /dev/null /etc/kdeskdash/secrets.env
sudo tee -a /etc/kdeskdash/secrets.env > /dev/null <<'EOF'
KVSCF_TOKEN=kvscf-<64hex>
KDESKDASH_TELEMETRY_REDISCLI_AUTH=<telemetry redis password>
KDESKDASH_KVSCF_REDISCLI_AUTH=<kvscf redis password — rpidash3 only>
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

Each device gets the secrets for *its own* kvscf: rpidash2 talks to cleo's, over
the open fleet claude-feed instance (no password); rpidash3 talks to kwork's,
over the second Redis instance on rpidash3 itself, which does require one. See
[docs/kwork-rpidash3-pairing.md](../../docs/kwork-rpidash3-pairing.md).
