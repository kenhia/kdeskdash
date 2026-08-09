<!-- kproject:begin — managed by kprojects/install.sh; do not edit inside this block -->
## kproject conventions

This project uses the kproject minimal harness
(`~/src/ai-agents/kprojects`). Keep context small; prefer doing over
ceremony.

### Layout

- `sprints/` — the project's evolution, one record per PR-sized unit of
  work (a "sprint")
  - `planning/` — planning docs; at minimum `roadmap.md` (the general plan)
  - `review/` — more formal reviews as the project matures
  - sprint records: `###-<short-name>.md` for small projects, or a
    `###-<short-name>/` directory of files for larger/more formal ones
  - a sprint record is one informal narrative: goal, decisions, what
    shipped, follow-ups — written during the sprint, not after
- `docs/` — project documentation, architecture, usage
- `.scratch/` — git-ignored scratch space for user or agent ephemera;
  use it instead of /tmp
- `justfile` — dev recipes; default recipe is `@just --list`; `just check`
  runs the CI gates; `just deploy` (or variants) if the project deploys
- `.env` — git-ignored; tokens and environment vars

### Workflow

- One sprint ≈ one PR. Sprint proposals and work items are managed in
  `korg`; durable cross-project knowledge goes in `klams`.
- If the korg or klams MCP tools are unavailable in your session, say so
  up front — don't silently work around missing infrastructure.
- TDD preferred: write the failing test first when practical.

### Tooling preferences

- Python managed by `uv`; lint/format with `ruff`; typecheck with `ty`
  (astral toolchain)
- License is MIT unless specifically directed otherwise
<!-- kproject:end -->

## Project

kdeskdash is a multi-mode, touch-enabled desk dashboard for the Raspberry Pi, built in C
with LVGL v9.2.2. It runs fullscreen on an 11.26" 1920×440 capacitive touch panel. Two
devices run the same generic-aarch64 build, both as user `ken`: `rpidash2` (Pi 5, dev
desk) and `rpidash3` (Pi 4, work desk). Per-device config lives in `deploy/hosts/<host>.env`;
secrets are hand-installed to `/etc/kdeskdash/secrets.env` and never committed. The
README is the canonical reference for hardware,
modes, env vars, Redis keys, and the systemd service — read it for anything user-facing.
This section covers what you need to *develop* here.

### Two build directories

There are two distinct CMake build trees. Keep them separate — do not run tests out of `build-pi`.

- **`build/`** — native host build. This is where the **unit tests** live and run
  (tests execute on the build host, so they are skipped when cross-compiling).
- **`build-pi/`** — aarch64 cross-compile for the actual Pi. Produces the deployable binary.

### Common commands

`just` wraps the usual loops (`just --list` for all of them):

```bash
just check                    # CI gate: build the host tree + run every unit test
just test golz                # run ONE test by name (ctest -R test_golz)
just publish                  # release: build + publish a version to the package store
just deploy [host] [version]  # install a published version (default rpidash2, newest)
just push-dev [host]          # dev loop ONLY: push this tree to a board, bypassing the store
just versions                 # what is published / cached here / running on each board
just sync-sysroot [host]      # one-time / after Pi apt changes: rsync a Pi sysroot to ~/pi-sysroot
just install-service [host]   # one-time per device: systemd unit + that host's env file
just golz-mc --help           # headless GoLZ balance sweep (Monte Carlo over the pure core)
```

The underlying commands, when you need them directly — note `-DKD_VERSION`: the
recipe passes the version stamp in (`scripts/version.sh`), CMake never derives
it, and a build configured without it stamps `unknown`, which `deploy` refuses.

```bash
cmake -B build -DKD_VERSION="$(scripts/version.sh)" && cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
cmake -B build-pi -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake -DKD_VERSION="$(scripts/version.sh)"
cmake --build build-pi --target kdeskdash -j"$(nproc)"
```

**Deploys go through the store** (sprint 024): a release is a versioned artifact
in the homelab package store, a deploy installs *that*, and naming an older
version is the rollback. The dev box does the fetching because the Pis stay
unmanaged. `docs/deploying.md` here; doctrine is k-homelab `docs/deploying.md`.
Store config (`KDESKDASH_STORE_URL`, `KDESKDASH_STORE_HOST`) lives in `.env`.

Adding a new source file to `kdeskdash` means editing `add_executable(kdeskdash ...)` in
`CMakeLists.txt`. Adding a test means a new `add_executable` + `add_test` block inside the
`if(NOT CMAKE_CROSSCOMPILING)` guard — link only the pure `.c` files under test, never LVGL.

### Architecture: pure cores + thin modes + a shell

The central discipline (and the user's stated preference — "less framework"): **business
logic lives in pure, host-tested C modules with no LVGL/Redis dependency; LVGL modes are
thin glue that render a core and wire touch.** Every non-trivial piece of logic should be
testable without hardware.

Read first: `src/mode.h` (the mode contract), `src/shell.c`, `src/main.c`, `CMakeLists.txt`.

- **Pure cores** (`src/gol.c`, `src/golz.c`, `src/stopwatch.c`, `src/calc.c`, `src/palette.c`, `src/registry.c`,
  `src/modeset.c`, `src/iconset.c`, `src/kvscf_feed.c`, `src/dev_telemetry.c`, `src/claude_feed.c`,
  `src/telemetry_host.c`, `src/bmp_write.c`, `src/clock_core.c`, `src/modes/dev_hostlist.c`,
  `src/modes/dev_view.c`) — no LVGL, no Redis, deterministic (RNG threaded through an
  explicit `uint32_t *state` seam). Each has a `tests/test_*.c`.
- **Modes** (`src/modes/*.c`) — each implements the `kd_mode_t` lifecycle from `src/mode.h`:
  `activate` / `deactivate` / `tick`, owning one LVGL screen and its private `state`. A mode
  does no ongoing work while deactivated. `*_mode_create(id, title)` builds and returns one.
- **Shared widgets** (`src/clock_widget.c` so far) — LVGL glue that is *not* a mode: takes a
  parent container, sizes itself to it, renders a pure core. The dual clock is one widget used
  by the Launcher's side pane and (WI #1136) the `clock` mode rebuild. Reach for this shape
  when two modes want the same thing on screen — not by generalizing an existing full-screen
  mode, which is how you get a widget shaped like whichever mode happened to be first.
- **Shell** (`src/shell.c`, `src/shell.h`) — owns the set of modes, the active mode, and
  gesture navigation: swipe left/right cycles content modes (wrapping), swipe down opens the
  Menu. It does **not** own mode storage; `main.c` keeps registered modes alive for the
  program's lifetime. A change callback (`shell_set_change_cb`) persists the active mode to Redis.
- **Entry** (`src/main.c`) — DRM display + evdev touch bring-up, registers the modes the
  modeset selects, wires the Redis handles the enabled modes actually use, runs the LVGL main
  loop until SIGINT/SIGTERM, tears down cleanly.

**Which modes a panel registers is configuration**, not a build flag: `src/modeset.c` parses
`KDESKDASH_MODES` (`"fun:<ids>;ops:<ids>"`), and its roster table is simultaneously the
built-in default, the default menu grouping, and the list of legal ids. A section's list
order is both the swipe-cycle order and the menu tile order — `menu.c` owns no id lists of
its own, so a device's set and its menu cannot drift apart. Every malformed spec degrades
(warn + skip; a spec selecting nothing falls back to the full set) because a blank panel is
only recoverable over SSH.

**Adding a mode is three lines**: the roster in `src/modeset.c`, a case in `main.c`'s
`create_mode()` dispatch, and its source in `CMakeLists.txt` — plus the mode's own `.c`/`.h`.

#### Four independent Redis handles — do not conflate them

Each has its own `redis_client_t` connection and failure isolation (a down endpoint never
stalls boot or another path). The generic client + backoff lives in `src/redis.c` /
`redis_internal.h`; each feed is a thin reader on its own handle:

1. **Control** (`src/redis.c`, `KDESKDASH_REDIS_*`) — remote mode control, last-mode
   persistence, GoL settings injection, screenshot trigger. Polled ~1×/sec from the main loop.
2. **Telemetry** (`src/telemetry.c`, `KDESKDASH_TELEMETRY_REDIS_*`) — read-only kpidash host
   metrics for Dev mode. Defaults to host `rpi53`.
3. **Claude feed** (`src/claude_redis.c`, `KDESKDASH_CLAUDE_REDIS_*`) — fleet Claude Code
   agent activity + usage limits, fed by `publisher/claude-pub.sh` hooks. Port 6380.
4. **kvscf feed** (`src/kvscf_redis.c`) — one handle, **two** readers: the `foreground`
   ("Remote") mode reads `kvscf:instances:*` / `kvscf:edge:*` / `kvscf:apps:*`, and `launcher`
   reads `kvscf:launcher:*`. Both **publish** to `kvscf:focus:<host>` (`{id}`, `{app}` or
   `{button}` — kvscf's precedence is `button` > `app` > `id`). Its own handle *and* its own
   endpoint config (`KDESKDASH_KVSCF_REDIS_*`), each field falling back independently to the
   Claude feed's — on rpidash2 both genuinely live on the same 6380 instance, but a panel can
   read the shared fleet Claude feed while driving a different kvscf. rpidash3 is that panel
   (sprint 026): it reads rpidash2's fleet feed and drives kwork's kvscf over a *second,
   password-protected* instance on rpidash3 itself (`deploy/redis-kvscf.conf`), which is why
   both gates are live there and why they fail in opposite ways — a bad
   `KDESKDASH_KVSCF_REDISCLI_AUTH` looks like an unreachable endpoint, a bad `KVSCF_TOKEN`
   looks like nothing at all. See `docs/kwork-rpidash3-pairing.md`. These are the only modes
   that **write/act on another machine**, gated by `KVSCF_TOKEN` (byte-exact, trimmed, never
   logged; per-kvscf-instance, so it lives in each device's `secrets.env`). PUBLISH rides the
   ordinary command connection — kdeskdash never SUBSCRIBEs.

Feeds are initialised **only for modes the modeset registered**, so a panel without Dev never
dials the telemetry endpoint at all. A handle shared by two modes is initialised when *either*
is registered — see the kvscf gate in `main.c`.

**A feed key's TTL is not a policy for every consumer.** The kvscf keys carry a 10s TTL, which
is right for a live window list (absent genuinely means "nothing to focus") and wrong for the
launcher's button layout (the machine publishing it sleeps and locks all day). `launcher`
therefore caches the last-good config and dims it rather than blanking, and
`kvscf_parse_launcher` only writes its `out` once a payload is known-good so there is no
window in which a half-parsed feed can erase a working layout.

### Key patterns (documented in `docs/solutions/best-practices/`)

Before touching simulations or LVGL gesture handlers, these capture hard-won decisions:

- **Two-layer faction reuse** (`two-layer-faction-reuse.md`) — GoLZ embeds an *unmodified*
  `gol_t` by value and adds parallel faction grids rather than widening the core's cell type.
  When building "the existing sim **plus** another interacting layer," compose — don't
  generalize the hot path. `gol_step` is byte-for-byte unchanged and shared by both modes; a
  parity test asserts the wrapped layer is bit-identical to bare `gol_step` with no zombies.
- **Swipe-vs-tap gesture guard** (`lvgl-swipe-vs-tap-gesture-guard.md`) — any `LV_EVENT_CLICKED`
  handler on a widget inside the swipe-navigated shell must guard against a swipe that
  released over it, or navigation and taps fight each other.
- **Adaptive feedback loop sets equilibrium** (`adaptive-feedback-loop-sets-equilibrium.md`)
  — see also the memory note: GoLZ's win ratio is pinned by the ±gens_to_win rule, not the
  machete params.
- **Draw only glyphs the font has** (`draw-only-glyphs-the-font-has.md`) — the vendored
  `SymbolsNerdFont-Regular.ttf` has **zero** emoji *and* zero Latin, Montserrat has no emoji
  (nor U+00B7), and `lv_font_t.fallback` cannot bridge the gap because TinyTTF reports every
  glyph as present. Filter text down to what the font actually has before drawing it — this
  applies to your own chrome, not just strings off the wire.
- **Grid children need cells immediately** (`lvgl-grid-children-need-cells-immediately.md`) —
  LVGL lays *hidden* children out too, so a pooled grid child with no cell, or a grid with no
  track descriptors, segfaults on the first layout pass. Install a placeholder 1×1 track set
  at build time, and keep the descriptor arrays in state (LVGL stores the pointer).
- **Sandboxing needs a second device** (`systemd-sandboxing-needs-a-second-device.md`) —
  `install-service` never overwrites a device's env file but *does* overwrite the unit, so a
  fleet drifts one device at a time. `PrivateTmp=yes` had been hiding device screenshots
  since sprint 010 and only surfaced when rpidash3 became the first host to run the committed
  unit. Re-run `install-service` everywhere after touching `deploy/kdeskdash.service`, and
  prefer `StateDirectory` for anything the outside world must read.

### Conventions

- **Sprint records carry the history.** `sprints/001-…` … `sprints/017-…` are the migrated
  plans (and, where one existed, the paired `requirements.md`) from the first 17 units of
  work; `sprints/018-multi-pi-deploy.md` is the first written natively under the kproject
  harness, and everything after it follows that shape — `ls sprints/` for the latest rather
  than trusting a number written here. New work gets a new
  `sprints/###-<short-name>.md` (or a directory if it warrants one). Durable lessons still
  go in `docs/solutions/`; don't delete `sprints/` or `docs/solutions/`.
- **Conventional commits** (`feat:`, `fix:`, `refactor:`, `docs:`), often scoped
  (`feat(golz): ...`). PRs are how work lands (`git log` is squash-merge PRs).
- **Colors come from the named palette** (`src/palette.h`, `KD_PAL_*` / `kd_pal_rgb`).
  New UI colors get an X-macro entry there (paint-store name + usage note) rather than
  a bare `lv_color_hex` literal; the `palette` mode displays the table on-panel.
  Existing modes' local `COLOR_*` blocks migrate opportunistically (korg WI 514).
- LVGL is a pinned submodule at `lib/lvgl` (v9.2.2); cJSON is vendored at `lib/cjson`.
  Clone with `--recurse-submodules`.

### Fonts

Body text uses the built-in Montserrat bitmap fonts (no font-conversion pipeline). The
`icons` mode is the exception: it renders Nerd Font glyphs at runtime via LVGL's
**TinyTTF** engine (`LV_USE_TINY_TTF` in `lv_conf.h`), reading the vendored
`fonts/ttf/SymbolsNerdFont-Regular.ttf` — nothing is baked. Two gotchas if you touch this:

- **libm is required** — TinyTTF's stb_truetype needs `sqrt`/`floor`/`pow`/… so `kdeskdash`
  links `m` (see `CMakeLists.txt`). Forgetting it is an obscure link error.
- **Load bytes yourself, use `lv_tiny_ttf_create_data`** — the `_create_file` path routes
  through LVGL's `lv_fs` drive-letter layer (no POSIX paths), which we don't register.
  `icons.c` reads the TTF into a state-owned buffer (kept alive for the mode's life, since
  create_data references it) and creates one font object per visible size.
- **Glyph-presence probing** — to filter sparse Nerd ranges, probe with a *cache-less*
  font (`create_data_ex(..., cache_size=0)`) and test `dsc.gid.index != 0`. The boolean
  return of `lv_font_get_glyph_dsc` is `true` even for missing glyphs, and a *cached* font
  logs `cache not allocated` per miss — the cache-less probe font avoids both traps.

Baking a curated subset the kpidash way (`lv_font_conv` → committed C font, for pixel-crisp
production icons) is the complementary path; the `icons` mode's favourites file is the
curation list that would feed it.
