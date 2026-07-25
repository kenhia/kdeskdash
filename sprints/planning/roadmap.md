# Roadmap

> The general plan for this project. Keep it current; detail lives in the
> sprint records.

Sprints 001–017 are the migrated history from the pre-kproject harness
(`docs/plans/` + `docs/brainstorms/`); 017 (palette mode) shipped 2026-07-21.
Everything below is the open korg backlog for `kdeskdash` — no sprint
proposal is currently active.

## Now

- (nothing in flight — pick from Next)

## Next

- **Pi 4 support + build variants for a second (kwork) instance** (korg WI 503, XL).
  Two intertwined efforts: (a) bring the app up on Pi 4 hardware — the app is
  software-rendered so there's no Pi 5 lock-in; the real gotchas are the DRM card
  number (`/dev/dri/card0` on Pi 4's vc4-kms-v3d, not `card1`) and the custom
  1920×440 HDMI timing; (b) introduce build/runtime variants so the kwork instance
  ships a control-plane mode set (no GoL, GoLZ, or dev graphs). Open questions for
  Ken are in the WI: same GeeekPi panel or a standard monitor, compile-time vs
  runtime variant selection, Pi 4 RAM SKU. Wants its own requirements doc before
  implementation.

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

- **kvscf control plane / Stream Deck replacement** (Part C of WI 503). The Remote
  ("foreground") mode is the seed; most of the underlying capability already exists
  on the kvscf side, so this is largely surfacing it through touch tiles — a
  configurable tile grid, more kvscf verbs beyond window-focus. Scope as its own
  epic once Pi 4 + variants land.

- Bake a curated Nerd Font subset the kpidash way (`lv_font_conv` → committed C
  font) for pixel-crisp production icons; the `icons` mode's favourites file is the
  curation list that would feed it.
