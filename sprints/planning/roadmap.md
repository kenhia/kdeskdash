# Roadmap

> The general plan for this project. Keep it current; detail lives in the
> sprint records.

Sprints 001–017 are the migrated history from the pre-kproject harness
(`docs/plans/` + `docs/brainstorms/`); 017 (palette mode) shipped 2026-07-21.
Everything below is the open korg backlog for `kdeskdash`.

## Now

- **WI 503 in flight** (see
  [multi-build-and-hardware.md](multi-build-and-hardware.md)). The rpidash3
  recon closed all three hardware risks (DRM card1, EDID-native 1920×440,
  by-id touch already the code default) and Ken's per-device mode-selection
  vision resolved Part B to **runtime config, single fat binary**. Two sprint
  proposals:
  1. ~~**Multi-Pi deploy + rpidash3 bring-up** (korg 666: WI 659–661)~~ —
     **sprint 018, shipped.** `just deploy <host>`, per-host env +
     `secrets.env` split, rpidash3 boot-to-dashboard on the full mode set.
     Record: [../018-multi-pi-deploy.md](../018-multi-pi-deploy.md). Two
     device-side follow-ups it left open: rpidash2 still runs a pre-sandboxing
     unit and wants an `install-service` re-run, and both panels want their two
     secrets (`KVSCF_TOKEN`, `KDESKDASH_TELEMETRY_REDISCLI_AUTH`) moved into
     `/etc/kdeskdash/secrets.env`.
  2. **Per-device mode sets (runtime variants)** (korg 667: WI 662–665) — next
     up. `KDESKDASH_MODES` pure-core parser, registration/menu wiring, kvscf
     endpoint split, curated sets live on both devices. The commented
     `KDESKDASH_MODES` lines are already sitting in `deploy/hosts/*.env`.

- **Calc post-live-test follow-ups** (korg WI 509, M). Deferred from sprint 016
  pending real desk use: trig (sin/cos/tan + inverses, deg/rad toggle), √x, 1/x,
  CE distinct from C, register persistence across restarts via Redis, register
  count/interaction tuning. The layout already reserves room for these.

- **Migrate modes' local `COLOR_*` defines onto palette names** (korg WI 514, M).
  Opportunistic: repaint each mode's `#define COLOR_*` block onto
  `kd_pal(KD_PAL_*)` so `src/palette.h` is the single source of truth, and retire
  the legacy clock/dev strays. Do it per-mode as modes get touched; verify on-panel
  with the `palette` mode.

## Later / Ideas

- **Configurable menu sections** (korg WI 668, M, after WI 663) — rename the
  Fun/Ops headers, 1/2/3 sections (18/9/6 tiles), palette-named label colors.
  First brick of the post-Launcher "generalize for others" push: with the new
  case design's STLs, anyone with a 3D printer and ~$150 in parts could build
  one (a third unit is planned for Ken's Dad, Christmas 2026).

- **kvscf control plane / Stream Deck replacement** (Part C of WI 503). The Remote
  ("foreground") mode is the seed; most of the underlying capability already exists
  on the kvscf side, so this is largely surfacing it through touch tiles — a
  configurable tile grid, more kvscf verbs beyond window-focus. Scope as its own
  epic once Pi 4 + variants land.

- Bake a curated Nerd Font subset the kpidash way (`lv_font_conv` → committed C
  font) for pixel-crisp production icons; the `icons` mode's favourites file is the
  curation list that would feed it.
