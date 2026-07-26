# Multi-build + hardware: two dashboards, one codebase

> Planning record for korg **WI 503** ("Pi 4 support + build variants for a second
> dashboard instance"). Written 2026-07-25, after the rpidash3 bring-up recon
> (WI 503 comment) and Ken's per-device mode-selection vision statement.
> Execution is split into two sprint proposals: **"Multi-Pi deploy + rpidash3
> bring-up"** (korg 666: WI 659–661) and **"Per-device mode sets (runtime
> variants)"** (korg 667: WI 662–665).

## The vision (Ken, 2026-07-25)

Each mode is **selectable per device via configuration**, including its menu
group and order. Working examples:

- **rpidash2** (Pi 5, dev desk): `fun:(GoL, GoLZ, Icons, Palette)`,
  `ops:(Claude, Remote, Clock, Dev, Calc)` — i.e. exactly today's menu.
- **rpidash3** (Pi 4, work desk): `fun:(Icons, Palette)`,
  `ops:(Remote, Clock, Dev, Calc, Launcher)` — "Launcher" is a future
  Stream-Deck-replacement extension of Remote/kvscf (WI 503 Part C), not in
  scope here.

Each device also has its own deploy config — rpidash3's Remote talks to a
**different kvscf** than rpidash2's.

## Hardware verdict: the Pi 4 delta is ~zero code

The recon (WI 503 comment, 2026-07-25) closed all three risks the original plan
flagged, and current code already handles the third:

| Concern | Outcome |
|---|---|
| DRM card number | **Identical to Pi 5**: `card0` = v3d (render-only), `card1` = vc4 display. `KDESKDASH_DRM_DEV=/dev/dri/card1` default holds on both. Probe-order artifact, so the env knob stays a knob. |
| 1920×440 HDMI timing | **Negotiated from EDID** with no firmware coaxing; panel enabled at native mode. Risk closed. |
| Touch evdev index | Moves (`event0` on Pi 4 vs `event1` on Pi 5), but the code default is already the stable by-id path (`config.c`: `/dev/input/by-id/usb-ILITEK_ILITEK-TOUCH-event-if00`). Identical on both. |
| Toolchain / OS | Same Debian 13 Trixie, same aarch64 userspace; only the kernel flavor differs (`-v8` vs `-2712`). The binary is generic aarch64 — **one build, both boards**. |
| Perf | **Measured at bring-up — caveat does not materialize.** Worst case (rgb composite, cell 2, 10ms step) is 75% of *one* A72 core on a 4-core box; ordinary GoL/GoLZ sits under 25%. |
| Thermals | **Measured — 60–67°C across ~25 min of mixed load, `get_throttled` 0x0 throughout.** Revisit once the panel is in its final enclosure. |

So there is no "Pi 4 port". The real work is **deploy targeting** and **mode-set
configuration**.

## Decisions

1. **Runtime mode selection, single fat binary** (resolves WI 503 Part B's
   compile-time-vs-runtime question). Ken's vision is configuration-driven
   selection per device, including group placement — that is runtime config by
   definition. Supporting reasons: `menu.c` already builds groups from id lists
   and skips unregistered modes, so the sparse case costs nothing; one artifact
   deploys to N devices (deploy stays dumb); binary size is irrelevant here; and
   a CMake variant matrix + `#ifdef`s in `main.c` is more framework for less.
   The compile-time "work doesn't get the code" property was a lean, not a
   requirement. **Confirmed by Ken 2026-07-26**: he'd initially meant build-time
   config, then talked himself out of it — the Pi binary is under 1.5MB with all
   nine modes (some of that core code), so even doubling the mode count wouldn't
   meaningfully grow it.
2. **One env var, pure-core parser.** `KDESKDASH_MODES="fun:<ids>;ops:<ids>"`,
   parsed by a new host-tested core (`src/modeset.c`). List order is both the
   swipe-cycle order and the menu tile order. Unset ⇒ the current full set, so
   rpidash2 is unchanged until its env opts in. Unknown id ⇒ warn + skip (a typo
   must never blank a panel). Two fixed groups, matching the menu; arbitrary
   groups can come later if ever needed.
3. **One build tree, host-parameterized deploy.** `build-pi/` stays the only
   cross tree; `just deploy <host>` / `just install-service <host>` call
   `scripts/deploy.sh` (which already takes a target) directly. One sysroot
   serves both boards (verify lib versions match after first rpidash3 sync);
   the `pi5-sysroot` name is parameterized or consciously shrugged (WI 659).
4. **Per-host committed env + uncommitted secrets.** `deploy/hosts/<host>.env`
   (endpoints, mode set — committed, no secrets) + a second optional
   `EnvironmentFile=-/etc/kdeskdash/secrets.env` line in the unit for
   `KVSCF_TOKEN` etc. `kdeskdash.env.example` remains the full-surface reference.
5. **kvscf gets its own endpoint config.** `KDESKDASH_KVSCF_REDIS_*`, falling
   back to the claude-feed values when unset (rpidash2 unchanged). Needed
   because rpidash3 reads the same fleet Claude feed (rpidash2:6380) but drives
   a different kvscf.
6. **Conditional feed init.** Only connect handles an enabled mode uses
   (dev → telemetry, claude → claude feed, foreground → kvscf). Isolation
   already made stray connections safe; this makes them not happen.

## Config surface per device (target state)

| Var | rpidash2 | rpidash3 |
|---|---|---|
| `KDESKDASH_MODES` | `fun:game_of_life,golz,icons,palette;ops:claude,foreground,clock,dev,calc` | `fun:icons,palette;ops:foreground,clock,dev,calc` (+`launcher` later) |
| `KDESKDASH_REDIS_*` (control) | local 6379 | local 6379 (install redis-server at bring-up) |
| `KDESKDASH_TELEMETRY_REDIS_*` | rpi53 | rpi53 (Dev mode is in its set) |
| `KDESKDASH_CLAUDE_REDIS_*` | local 6380 | `rpidash2:6380` (shared fleet feed) — confirmed 2026-07-26 |
| `KDESKDASH_KVSCF_REDIS_*` | (unset → claude values) | the work-side kvscf instance — value TBD until it exists |
| `KVSCF_TOKEN` | cleo kvscf token (secrets.env) | work kvscf token (secrets.env) |
| DRM / touch | defaults | defaults (verified identical) |

## Sprint 1 — Multi-Pi deploy + rpidash3 bring-up (korg 666) — **shipped**

Record: [../018-multi-pi-deploy.md](../018-multi-pi-deploy.md).

- [x] **WI 659** — host-parameterized `just deploy <host>` / `install-service
  <host>`; sysroot naming; README build section.
- [x] **WI 660** — `deploy/hosts/*.env` + `secrets.env` split in the unit;
  install-service installs the matching host file.
- [x] **WI 661** — rpidash3: ssh from build host, apt deps (libhiredis,
  libdrm-dev/libhiredis-dev for sysroot, redis-server), install-service,
  deploy, reboot test, full-mode on-panel smoke, GoL-perf + thermals
  observations recorded.

Exit met: rpidash3 boots to the dashboard running the same full build as
rpidash2. Two things the plan did not anticipate, both in the record: the
telemetry Redis AUTH is a **second secret** (it was living in rpidash2's config
file), and the unit's `PrivateTmp=yes` hid device screenshots from `kddss` —
fixed by moving the default shot path to the state directory. rpidash2 still runs
a pre-sandboxing unit and wants an `install-service` re-run alongside its secrets
migration.

## Sprint 2 — Per-device mode sets (korg 667)

- [ ] **WI 662** — `src/modeset.c/h` pure core + `tests/test_modeset.c`
  (grammar, defaults, dedupe, unknown-id, ordering).
- [ ] **WI 663** — registration table in `main.c`, menu groups sourced from the
  modeset (FUN_IDS/OPS_IDS move in as the built-in default), conditional feed
  init, active-mode-restore fallback verified. Byte-identical when unset.
- [ ] **WI 664** — `KDESKDASH_KVSCF_REDIS_*` with claude-feed fallback.
- [ ] **WI 665** — curated sets live on both devices, README + roadmap updated.

Exit: each panel shows exactly its configured mode set; rpidash2 visually
unchanged; adding the future Launcher mode to rpidash3 is a one-line env edit
plus the mode itself.

## Out of scope / later

- **Launcher mode** (WI 503 Part C, Stream-Deck replacement): own brainstorm +
  epic once these two sprints land. Primarily opening web sites and launching
  programs — the main way Ken uses the Stream Deck today. The modeset makes its
  per-device rollout free.
- **Configurable menu sections** (WI 668, after WI 663): rename the Fun/Ops
  headers; 1/2/3 sections (18/9/6 tiles each); per-section label colors from
  the named palette. WI 662's parser keeps the seam open (section list out of
  the parse, fun/ops not baked into the API shape).
- **Generalize the repo for others** (the push after Launcher lands): with the
  new case design's STLs, anyone with a 3D printer and ~$150 in parts could
  build one. A third unit for Ken's Dad (Christmas 2026) rides the
  `deploy/hosts/` pattern as-is. Per-mode endpoints are already covered once
  WI 664 lands (dev → telemetry, claude → claude feed, foreground → kvscf;
  control Redis is app-level).
- **Named Redis-endpoint registry** (considered 2026-07-26, deferred): instead
  of per-feed env vars, a registry of named endpoints (`control`, `fleet`,
  `work`, …) that modes bind to by name (or index). The right shape if the
  generalization push multiplies feeds/modes — one place for host/port/auth,
  no var sprawl — but per-feed env vars are sufficient at today's four handles.
  Revisit alongside the generalization push.
- Baked font subset, GoL perf tuning for A72 — only if bring-up observations
  demand it.

## Open questions (non-blocking, resolve at flip time)

- rpidash3's kvscf endpoint + token, once the work-side kvscf exists (WI 664).
- Final rpidash3 mode set — Ken adjusts the env line at WI 665 deploy.
