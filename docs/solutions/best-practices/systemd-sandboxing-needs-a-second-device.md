---
title: Systemd sandboxing that has only ever been committed is untested sandboxing
date: 2026-07-26
category: docs/solutions/best-practices
problem_type: best_practice
module: deploy (systemd unit, install-service, device screenshots)
component: deployment
severity: medium
applies_when:
  - Adding or tightening a directive in deploy/kdeskdash.service
  - install-service only writes files "if absent", so a device can drift from the repo
  - A host-side script reads a path the service writes
  - Bringing up a second device from a unit no existing device is actually running
related_components:
  - deploy/kdeskdash.service
  - scripts/deploy.sh
  - scripts/kddss
  - src/screenshot.c
tags: [systemd, sandboxing, privatetmp, deploy, multi-device, drift]
---

# Systemd sandboxing that has only ever been committed is untested sandboxing

## Context

Sprint 010 hardened `deploy/kdeskdash.service`: `ProtectSystem=strict`,
`ProtectHome=read-only`, `PrivateTmp=yes`, `StateDirectory=kdeskdash`. All
correct, all committed, all reviewed.

Sprint 018 brought up a second panel (rpidash3) and `scripts/kddss` — the device
self-screenshot wrapper — failed there while continuing to work on rpidash2.

The cause was `PrivateTmp=yes`. The app wrote its shot to
`/tmp/kdeskdash-shot.bmp`; with a private `/tmp` that file actually lands in
`/tmp/systemd-private-<id>-kdeskdash.service-XXXXXX/tmp/`, invisible to `kddss`
or anything else outside the service's mount namespace. The screenshot had been
rendering perfectly and going somewhere nobody could reach.

The reason nobody noticed for eight sprints is the interesting part:
**`install-service` installs the unit but `rpidash2` had not had it re-run since
the hardening landed.** The one device in the fleet was still running the
*pre-sandboxing* unit. The repo said one thing, the running system did another,
and every test passed because the tests exercised the running system. rpidash3
was the first host ever to receive the committed unit — so the second device is
what surfaced a bug that had been latent since the day the hardening merged.

## Guidance

**1. A directive that changes the process's view of the filesystem needs a
matching look at every path the app and its tooling touch.** `PrivateTmp`,
`ProtectSystem`, `ProtectHome`, `PrivateDevices` and friends do not fail loudly
— they silently relocate or hide things. When adding one, enumerate the paths
crossing the service boundary: config in, state out, screenshots out, sockets,
device nodes. `/tmp` is the classic trap because it looks shared and isn't.

**2. Prefer `StateDirectory` for anything the outside world needs to read.**
Under `ProtectSystem=strict` it is the one path that is both writable by the
service and visible on the host filesystem. That is why
`SCREENSHOT_DEFAULT_PATH` is now `/var/lib/kdeskdash/kdeskdash-shot.bmp` — the
fix cost nothing in hardening, unlike `PrivateTmp=no`.

**3. Re-run `install-service` on every device after changing the unit.** It is
deliberately conservative — it never overwrites `/etc/kdeskdash/kdeskdash.env`,
because hand edits on a panel are precious — but it *does* overwrite the unit.
Deploying a binary does not update the unit, so a fleet drifts one device at a
time and the drift is invisible until something breaks asymmetrically.

**4. Make host-side tooling tolerate both the old and new layout during a
rollout.** `kddss` tried the state directory and then fell back to `/tmp`, so it
worked against a device that had not been redeployed yet. In a multi-device
fleet "all devices are on the current build" is a goal, not an invariant.

*(Sprint 039 dropped that fallback, and the reason is the rule rather than an
exception to it: the trigger moved to `kdash:panelshot:<host>` on central, and a
panel still running a build old enough to write `/tmp` does not answer that key
at all. The fallback stopped covering anything — which is the test to apply
before removing one, not "it looks unused".)*

## Signature

If a device-side script that works on one host silently produces nothing on
another, and the service itself looks healthy:

```bash
systemctl show kdeskdash -p PrivateTmp -p ProtectSystem -p StateDirectory
sudo sh -c 'ls -l /tmp/systemd-private-*kdeskdash*/tmp/'
diff <(sudo cat /etc/systemd/system/kdeskdash.service) deploy/kdeskdash.service
```

The last one is the general check: **what the device runs versus what the repo
says it should run.** Run it on every device before concluding a unit directive
is well tested.
