# Sprint 018 — Multi-Pi deploy + rpidash3 bring-up

> korg sprint proposal **666**, covering WI **659** (host-parameterized deploy +
> sysroot naming), **660** (per-host env files + secrets split), **661**
> (rpidash3 bring-up + full-set smoke test). First half of WI 503; plan:
> [planning/multi-build-and-hardware.md](planning/multi-build-and-hardware.md).
> Branch `feat/multi-pi-deploy`.

## Goal

Make the deploy path take a target host, give each device its own committed
config with secrets held out of the repo, and bring rpidash3 (Pi 4, 8GB) up
boot-to-dashboard running the same build as rpidash2.

The recon in WI 503's comment had already closed the hardware questions, so this
sprint was never a "Pi 4 port" — the binary is generic aarch64 and the two boards
present identically. What it actually took was deploy plumbing, a config split,
and finding out what breaks the first time a device gets the *current* committed
systemd unit.

## What shipped

### Host-parameterized deploy (WI 659)

`just deploy [host]` / `just install-service [host]` / `just sync-sysroot [host]`,
all defaulting to `rpidash2` and calling `scripts/deploy.sh` (which already took
an ssh target) directly. The recipes no longer route through the CMake custom
targets, which were only wrappers; those targets stay for back-compat with the
`KDESKDASH_TARGET` cache variable, and `install-service` now derives the host
from it to pick the right env file.

`install-service` also lost its `build-pi` dependency — it installs a unit and an
env file and never needed the binary.

Sysroot renamed `PI5_SYSROOT` → `PI_SYSROOT`, `~/pi5-sysroot` → `~/pi-sysroot`,
in both `scripts/sync-sysroot.sh` and `cmake/aarch64-toolchain.cmake`. Both keep
a back-compat chain: an explicit `PI5_SYSROOT` wins, and an existing
`~/pi5-sysroot` with no `~/pi-sysroot` is used as-is, so nobody is forced into a
re-sync by a rename. (Consequence: the old directory is never migrated on its
own. Deliberate — deleting it is a one-line manual step whenever you want it.)

One sysroot really does serve both boards. Verified rather than assumed:
`libdrm-dev`/`libdrm2` at `2.4.131-1~bpo13+1+rpt2` and `libhiredis` at
`1.2.0-6+b3` on rpidash2 and rpidash3 alike, both Debian 13 Trixie. Only the
kernel flavor differs (`-2712` vs `-v8`) and that never reaches the linker.

### Per-host env + secrets split (WI 660)

`deploy/hosts/rpidash2.env` and `deploy/hosts/rpidash3.env` — committed,
no secrets, one file per device. `install-service <host>` installs the matching
file to `/etc/kdeskdash/kdeskdash.env`, still only when absent so hand edits on
the panel survive. A host with no file yet falls back to the example and says so,
so adding a device never hard-fails.

The unit grew a second `EnvironmentFile=-/etc/kdeskdash/secrets.env`, read after
the config file (so a secret can also override a committed value). Both entries
keep systemd's leading `-`, so a device missing either still boots.

`deploy/kdeskdash.env.example` stays the full-surface reference and picked up the
`KDESKDASH_CLAUDE_REDIS_*` trio, which it had never documented.

**Two secrets, not one.** The plan named `KVSCF_TOKEN`. Bring-up found a second:
rpi53's telemetry Redis answers `NOAUTH Authentication required`, so
`KDESKDASH_TELEMETRY_REDISCLI_AUTH` is a credential too — and it was sitting in
rpidash2's live `/etc/kdeskdash/kdeskdash.env` alongside the kvscf token. Both
now belong in `secrets.env`; `deploy/hosts/README.md` documents the install with
what degrades if you skip it.

### rpidash3 bring-up (WI 661)

Passwordless `ssh ken@rpidash3` from the build host worked out of the box (the
recon had only proven it from cleo). Installed `libhiredis1.1.0` (runtime — it
was missing), `libdrm-dev` + `libhiredis-dev` (sysroot), `libdrm-tests`, and
`redis-server` for the local control instance.

`just install-service rpidash3` + `just deploy rpidash3`, then a reboot test:
service `enabled`, auto-started ~24s after reboot, `NRestarts: 0`, and it
restored its last active mode from the local Redis. Boot-to-dashboard holds.

Hardware confirmations, all matching the recon: `card1` is the vc4 display
(`card0` = render-only v3d), the connector reports `1920x440` first among its
modes and is `connected`, and
`/dev/input/by-id/usb-ILITEK_ILITEK-TOUCH-event-if00 -> ../event0` resolves the
code's default touch path. No env override needed for either.

All nine modes activated and rendered, driven through `kdeskdash:active_mode` and
captured with `kddss`. Notable ones: **Claude** reads rpidash2:6380 over the LAN
and shows the live fleet, which validates the cross-host feed config; **Remote**
lists fleet windows and correctly reports `no token · view only` with no
`secrets.env` installed; **Dev** activates but has no host data, pending the
telemetry AUTH secret. Ken drove touch and swipe navigation on the panel by hand
during the session.

### Bug found by bring-up: PrivateTmp hid the screenshot

`kddss` failed on rpidash3 while working on rpidash2. Cause: the unit sets
`PrivateTmp=yes`, so the shot the app wrote to `/tmp/kdeskdash-shot.bmp` landed
in `/tmp/systemd-private-…-kdeskdash.service-*/tmp/`, invisible to anything
outside the service. It worked on rpidash2 only because that device still runs a
*pre-sandboxing* unit — `install-service` had not been re-run there since the
hardening landed. rpidash3 was the first host to receive the current committed
unit, so it was the first to hit it.

Fix: `SCREENSHOT_DEFAULT_PATH` moves to `/var/lib/kdeskdash/kdeskdash-shot.bmp`.
`StateDirectory=kdeskdash` is the one path that is both writable under
`ProtectSystem=strict` and visible on the host filesystem, so no sandboxing is
given up. `kddss` tries the state directory then falls back to `/tmp`, keeping it
working against a device that has not been redeployed yet.

Worth remembering as a general shape: **sandboxing that has never been installed
anywhere is untested sandboxing.** The unit had been correct in the repo and
wrong in effect for however long rpidash2 went without an `install-service` run.

## Perf + thermals on the A72 (observations, not gates)

WI 503 flagged GoL/GoLZ `rgb 1` as the one thing that might not hold on a Pi 4.
Measured as CPU time against wall time from `/proc/<pid>/stat`, at
`LV_DEF_REFR_PERIOD 33` (~30fps target):

| Configuration | CPU | Temp |
|---|---|---|
| Clock (idle-ish baseline) | 32.5% of one core | ~60°C |
| GoL, single board, 120ms step | 23.2% | ~61°C |
| GoL, rgb composite, 120ms step | 19.7% | ~61°C |
| GoL, rgb composite, cell 6, 10ms step | 52.5% | 65.2°C |
| **GoL, rgb composite, cell 2, 10ms step** (worst case) | **75.1%** | 67.2°C |
| GoL, mono, cell 2, 10ms step | 59.7% | 66.2°C |
| GoLZ, defaults | 7.7% | ~61°C |

The caveat does not materialize. Even the worst case anyone can ask for — three
composited boards at 960×220 cells stepping as fast as the mode allows — stays
inside a single A72 core with a quarter to spare, on a 4-core box. `get_throttled`
read `0x0` throughout ~25 minutes of mixed load, temperature never left 60–67°C,
and the SoC clocked back to 600–900MHz between bursts, i.e. it was not being
asked to work hard. No case/cooling change is indicated by these numbers, though
the final enclosure will change the picture.

(The clock-mode baseline reading above its own GoL numbers is a curiosity worth a
glance sometime — probably the large font repaint invalidating a big area every
second. Not chased here.)

## Both devices migrated

Done in the same session, at Ken's go-ahead:

- **rpidash2** — its two secrets extracted from `/etc/kdeskdash/kdeskdash.env`
  into `secrets.env` (0600, root), the old combined file kept as
  `kdeskdash.env.pre-sprint018.bak`, then `just install-service rpidash2` laid
  down the current sandboxed unit and the committed `deploy/hosts/rpidash2.env`.
  Redeployed, `NRestarts: 0`, and the running process shows both env files
  loaded — committed config vars plus `KVSCF_TOKEN` and
  `KDESKDASH_TELEMETRY_REDISCLI_AUTH`. The dev desk is off the pre-sandboxing
  unit at last, which is what made the `PrivateTmp` bug invisible.
- **rpidash3** — `secrets.env` installed with the telemetry AUTH only (piped
  host-to-host, never printed). Verified from the panel: the telemetry Redis
  answers and Dev mode's host list populates (kai, kubs0, kubsdb) where it was
  empty before. Assigning hosts to the charts is a tap on the panel.

rpidash3 has no `KVSCF_TOKEN` on purpose — the work-side kvscf does not exist
yet, and cleo's token would let the work panel focus windows on the dev machine.

## Follow-ups

- **rpidash3's kvscf endpoint + token** stay open until the work-side kvscf
  exists; the endpoint split itself is WI 664 in the next sprint.
- `KDESKDASH_MODES` lines sit commented in both host env files, ready for korg
  667 (WI 662/663) to make them live.
- Delete `~/pi5-sysroot` and re-sync to `~/pi-sysroot` whenever convenient — the
  back-compat fallback means nothing forces it.
