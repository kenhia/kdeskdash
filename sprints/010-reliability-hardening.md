# feat: reliability bug-bash + hardening

Origin: korg sprint proposal 175 ("Reliability bug-bash -- klams + kdeskdash"),
kdeskdash slice. Five deferred `/ce-review` follow-ups, no separate brainstorm.
Branch `feat/reliability-hardening`. korg items #49-#53.

Ordering is host-testable-first: the three pure/software changes (#53, #50, #52
logic) land and prove out under `ctest` on kai; the two changes needing the panel
(#51, #49) are implemented here but carry a **rpidash2 hardware-verify** gate
before the sprint ships.

## #53 -- make `redis_client_t` opaque (stop leaking hiredis)   [refactor]

`redis.h` `#include <hiredis/hiredis.h>` and exposes a transparent struct, so
every includer (main.c, dev.c, game_of_life.c, golz.c) transitively pulls in
hiredis though only the Redis units touch a context.

- New `src/redis_internal.h`: the hiredis include + the full
  `struct redis_client { redisContext *ctx; ... }` definition.
- `redis.h`: drop the hiredis include, forward-declare
  `typedef struct redis_client redis_client_t;`, keep only public prototypes.
- The three implementation units that embed the handle by value and touch
  `.ctx` -- `redis.c` (g_control), `telemetry.c` (g_tlm), `claude_redis.c`
  (g_cf) -- `#include "redis_internal.h"`. (Approach (b) from the item: internal
  header shared by the impl units, no heap churn.)
- Done when main.c/dev.c/game_of_life.c/golz.c no longer see hiredis and the
  host + cross builds stay clean.

## #50 -- extract testable GoL settings validation   [refactor + test]

`apply_field()` in `redis.c` validates **untrusted** `kdeskdash:gol:settings`
values but is trapped in the hiredis-coupled TU, so its per-field clamps (which
already drifted once on `cell_size`) can't be host-tested.

- New pure unit `src/gol_settings.{c,h}`:
  `bool gol_settings_apply_field(gol_settings_t *cfg, const char *field, const char *val)`.
  No hiredis; depends only on gol.h for the struct.
- `redis.c` `apply_field()` keeps the hiredis HGETALL iteration and delegates
  each (field,val) to the pure fn.
- `atoi`/`atof` -> `strtol`/`strtod` with endptr + ERANGE checks. Behavior note:
  valid input is unchanged; junk (non-numeric) is now **rejected** (leaves the
  caller's randomized default) instead of silently parsing to 0 -- stricter and
  the point of the extraction.
- `tests/test_settings.c` boundary cases: cell_size 1/2/64/65, padding -1/0/16/17,
  density 0.0/eps/1.0/>1.0, trail_turns + speed_ms bounds, unknown field (no-op),
  non-numeric value. Wired into the `NOT CMAKE_CROSSCOMPILING` host-test block;
  gol_settings.c added to the main executable sources.

## #52 -- dev-side WAITING state   [behavior + test; HW-verify]

`dev_side_resolve()` (`src/modes/dev_view.c`) has no state for a host that is
assigned + discovered + reachable but has never produced a valid sample:
`ever_live` stays false and it renders as a flat-zero LIVE chart -- a broken host
looks healthy pinned at 0%.

- Add `DEV_SIDE_WAITING` to `dev_side_view_t`, ranked for
  `assigned && seen_in_discovery && telemetry_ok && !ever_live`.
- Render via `view_msg()` in `src/modes/dev.c` ("waiting for data"), freezing the
  empty charts like the other non-live states. EMPTY/OFFLINE/STALE/UNAVAIL
  semantics untouched.
- `tests/test_dev_view.c`: new-state case + the WAITING->LIVE transition on first
  OK sample.
- HW-verify (rpidash2): assign a host publishing malformed/no telemetry, confirm
  "waiting for data", then confirm LIVE on first good sample.

## #51 -- bound DNS resolution   [reliability; HW-verify]

`redisConnectWithTimeout()` bounds the TCP connect poll but **not**
`getaddrinfo()`; all Redis I/O is on the single LVGL UI thread, so a dead resolver
freezes the dashboard well past the 250ms budget.

- Resolve the endpoint name to an IP once at init (cache on the handle),
  reconnect against the cached IP so the render path never calls `getaddrinfo`.
  Shared `redis_client_t` path, so control + telemetry both benefit. Per-handle
  backoff behavior preserved.
- HW-verify (rpidash2): point the telemetry host at an unresolvable name, confirm
  the dashboard stays responsive.

## #49 -- harden the systemd unit   [config; HW-verify]

`deploy/kdeskdash.service` runs `User=root`, `Restart=always`/`RestartSec=2`, no
start-limit and no sandboxing.

- `StartLimitIntervalSec=0` (never permanently give up restarting -- verify key
  placement on Trixie: newer systemd wants it in `[Unit]`).
- `NoNewPrivileges=yes`, `ProtectSystem=strict` (+ `ReadWritePaths=` for anything
  required), `ProtectHome=read-only`, `PrivateTmp=yes`.
- Must not break DRM master (`/dev/dri/*`) / evdev (`/dev/input/*`) / touch --
  validate `DeviceAllow=`/`PrivateDevices` interactions.
- HW-verify (rpidash2): reboot-to-dashboard + a rapid-crash-loop check that the
  unit doesn't wedge in `failed`.

## Ship gate

Host `ctest` green on kai covers #53/#50/#52-logic. #52-render, #51, #49 need
rpidash2 before `/sprint-ship`. The klams slice of proposal 175 (#54/#55/#56/#31)
is a separate start-sprint run on kubs0.
