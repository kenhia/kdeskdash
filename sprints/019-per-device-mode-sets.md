# Sprint 019 — Per-device mode sets (runtime variants)

> korg sprint proposal **667**, covering WI **662** (modeset pure core), **663**
> (registration/menu wiring + conditional feed init), **664** (kvscf endpoint
> split), **665** (curated sets live + docs). Second half of WI 503; plan:
> [planning/multi-build-and-hardware.md](planning/multi-build-and-hardware.md).
> Branch `feat/per-device-mode-sets`.

## Goal

Let each panel declare which modes it carries, in what order, under which Menu
section — from configuration, not a build flag. Part B of WI 503, whose
compile-time-vs-runtime question was settled during planning: **runtime config,
single fat binary**.

## What shipped

### The grammar and its pure core (WI 662)

`src/modeset.{c,h}` — no LVGL, no Redis, no allocation, `tests/test_modeset.c`
alongside it:

```
KDESKDASH_MODES="fun:game_of_life,golz,icons,palette;ops:claude,foreground,clock,dev,calc"
```

A section's list order is **both** the swipe-cycle order and the Menu tile order.
Whitespace is tolerated, empty sections are legal, a repeated section name merges
rather than duplicating, and duplicate ids keep the first.

One table in `modeset.c` — the roster — is simultaneously the built-in default
set, the default grouping, and the list of legal ids. That triple duty is the
point: a new mode is named there once, in `main.c`'s constructor dispatch, and in
`CMakeLists`. It used to also need a line in `menu.c`, and the two could drift.

**Every malformed path degrades rather than blanking a panel.** An unknown id or
section name warns on stderr and is skipped; a spec that selects *nothing* usable
falls back to the full default set. A panel you cannot navigate is only
recoverable over SSH, so "a typo must never blank the panel" is the core's
central invariant, and each path has a test.

The parse emits a *section list* — `(name, first, count)` — rather than two
fields called fun and ops. Nothing in the API shape assumes two sections or those
names, so making them configurable (korg WI 668) will not reshape any consumer.
`MODESET_MAX_SECTIONS` is already 3.

### Wiring (WI 663)

`main.c`'s nine hardcoded `shell_register_content_mode` calls became a loop over
the modeset, registering only enabled modes in declared order.

The WI called for a `{id, title, create_fn}` table; the constructors do not share
a signature (icons and foreground need paths from `cfg`), and three adapter shims
to force uniformity would have cost more than they saved. It is an
id→constructor dispatch instead — same single-place-per-mode property, and it
reads almost exactly like the block it replaced.

`menu.c` lost `FUN_IDS`/`OPS_IDS` entirely. Sections, membership, order and the
header text now come from the modeset, so a per-device set and the menu it draws
cannot disagree. The old safety net survives, generalised: a content mode the
modeset never mentions still lands in the last section rather than vanishing.

Feeds initialise only for modes that actually registered — no Dev means the
telemetry endpoint is never dialled and never backed off against. Failure
isolation already made a stray connection *harmless*; this makes it not happen.

`shell_start` needed no change: `shell_find_mode` returns NULL for a
now-disabled mode, which already falls through to the Menu. Verified rather than
assumed, as the WI asked.

### kvscf endpoint split (WI 664)

`KDESKDASH_KVSCF_REDIS_HOST/PORT/AUTH`, each falling back **independently** to
the claude-feed value — set only the host and you inherit the claude port and
auth. rpidash2 needs no change, because there the kvscf keys genuinely do live on
the claude-feed instance. rpidash3 is why the seam exists: it reads the shared
fleet Claude feed on rpidash2:6380 while needing to drive a *different* kvscf.

With this, every feed-backed mode owns its endpoint config — dev → telemetry,
claude → claude feed, foreground → kvscf. The control Redis stays app-level.

### Live on both devices (WI 665)

Both `deploy/hosts/*.env` carry explicit `KDESKDASH_MODES` lines now.

- **rpidash2** — its full complement, stated rather than implied:
  `fun:game_of_life,golz,icons,palette;ops:claude,foreground,clock,dev,calc`.
- **rpidash3** — `fun:icons,palette;ops:foreground,clock,dev,calc`. No
  simulations, no fleet agent view; the work desk earns its space with
  control-plane and utility modes. GoL/GoLZ are omitted **by preference, not
  capability** — sprint 018 measured the A72 worst case at 75% of one core. The
  future Launcher mode joins that ops list when it exists, as a one-line edit.

## Verification

Menus were compared as PNG hashes, not by eye:

| Check | Result |
|---|---|
| rpidash3, var unset, vs. the pre-change build | **byte-identical** (`7570062f…`) |
| rpidash3, deliberately garbage spec | **byte-identical to unset** — the fallback really does render the full menu |
| rpidash3, curated 6-mode set | exactly Icons/Palette + Remote/Clock/Dev/Calc |
| rpidash2, before vs. after its flip | **byte-identical** (`7570062f…`) — WI 665's "visually unchanged" is pixel-exact |

Degradation was exercised on hardware, not just in tests:
`fun:icons,nosuchmode,golz;ops:clock` logged `unknown mode "nosuchmode" —
skipped` and registered 3; `total-garbage` logged the no-usable-modes fallback
and drew the full menu.

`just check` 16/16 (test_modeset is the sixteenth).

## Decisions taken during the sprint

**Swipe order and menu order now agree, which changes rpidash2 slightly.** Before
this sprint the swipe cycle followed `main.c`'s registration order (GoL, GoLZ,
Clock, Dev, Claude, Icons, Remote, Calc, Palette) while the menu followed
FUN/OPS — two different orders nobody had noticed diverging. One list cannot
express both, and the grammar's whole premise is that list order *is* the order.
Ken chose to accept the convergence: swipe now runs Fun-then-Ops. Worth noting
this was not really optional — rpidash2's own explicit config line produces that
same order, so the change arrived with WI 665 either way.

The rejected alternative was non-contiguous section slices (~30 more lines in
modeset.c) to keep the two orders divergent.

## Bug found on the way

**Under systemd every startup `printf` was invisible until shutdown.** stdout is
a pipe there, so glibc block-buffers it; the 4KB buffer never filled, and
`journalctl -u kdeskdash` showed nothing from a running service until it exited
and flushed. It had been true since the service was first installed — the new
"mode set from KDESKDASH_MODES (N modes)" line is simply the first startup
diagnostic anyone went looking for in the journal. Fixed with `setvbuf(stdout,
NULL, _IOLBF, 0)` at the top of `main`, matching stderr's already-unbuffered
warnings. Every past startup line benefits.

## Process note

The branch spent a day parked while `main` was checked out from outside the
session, so the whole sprint sat uncommitted in the working tree. No work was at
risk — the branch had never diverged from `main`, so the checkout carried the
changes across rather than stranding them — but the lesson is to commit a
sprint's first coherent slice early rather than at the end.

## Follow-ups

- **rpidash3's kvscf endpoint + token** remain the one deferred item: the
  work-side kvscf does not exist yet. Its env file carries commented
  `KDESKDASH_KVSCF_REDIS_*` lines ready to uncomment, and the token belongs in
  that device's `secrets.env` since it is per-kvscf-instance.
- **korg WI 668** (configurable menu sections — names, 1/2/3 count, per-section
  palette colours) is the natural next brick, and the seam is already open:
  `header_text()` in `menu.c` is the one place that assumes a section name needs
  title-casing rather than being author-supplied verbatim.
- The **Launcher** mode (WI 503 Part C) now rolls out per-device for free.
