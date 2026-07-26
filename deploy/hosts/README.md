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

Two variables must never reach the repo, so they live in a second file the unit
reads optionally, hand-installed once per device:

| Var | What it unlocks | Missing ⇒ |
|---|---|---|
| `KVSCF_TOKEN` | Remote mode's window-focus commands | window list still shows, mode says "view only" |
| `KDESKDASH_TELEMETRY_REDISCLI_AUTH` | the kpidash telemetry Redis on rpi53, which requires AUTH | Dev mode activates but shows no host data |

```bash
sudo install -d -m755 /etc/kdeskdash
sudo install -m600 /dev/null /etc/kdeskdash/secrets.env
sudo tee -a /etc/kdeskdash/secrets.env > /dev/null <<'EOF'
KVSCF_TOKEN=kvscf-<64hex>
KDESKDASH_TELEMETRY_REDISCLI_AUTH=<telemetry redis password>
EOF
sudo systemctl restart kdeskdash
```

The unit lists this file after the config file, so a value set here also
overrides the committed one. Both entries use systemd's leading `-`, so a device
without the file starts fine — it just runs with those two features degraded as
above.

Each device gets the token for *its own* kvscf: rpidash2 talks to cleo's,
rpidash3 to the work-side one.
