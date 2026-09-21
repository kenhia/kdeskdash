<!-- kproject:begin — managed by kprojects; do not edit inside this block -->
## kproject conventions

This project uses the kproject minimal harness
(<https://github.com/kenhia/kprojects>). Keep context small; prefer doing
over ceremony.

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
- Mark each work item resolved as its work completes — don't batch the
  resolutions into sprint-ship. A proposal's progress should be readable
  while the sprint is running, which is the only time it is useful.
- If the korg or klams MCP tools are unavailable in your session, say so
  up front — don't silently work around missing infrastructure.
- TDD preferred: write the failing test first when practical.

### Tooling preferences

- C/C++ built with `cmake`: configure out-of-source, build with
  `cmake --build`, test with `ctest`
- `ctest` prints "No tests were found!!!" and **exits 0** when nothing is
  registered — pass `--no-tests=error` (CMake ≥ 3.20) or the gate passes
  loudest when there is least to check (same trap as `gofmt -l`)
- The configure flags are the project's own. If the repo documents a
  tests-only or native-CI mode, the gate uses that — a gate needing a
  cross-compiler, a sysroot or hardware is a gate nobody runs
- No formatter in the gate: `clang-format` asserts nothing without a
  committed `.clang-format`. Add `just fmt` once the repo has one
- License is MIT unless specifically directed otherwise
<!-- kproject:end -->

## Project

kdeskdash is a multi-mode, touch-enabled desk dashboard for the Raspberry Pi, built in C
with LVGL v9.2.2. It runs fullscreen on an 11.26" 1920×440 capacitive touch panel. Two
devices run the same generic-aarch64 build, both as user `ken`: `rpidash2` (Pi 5, dev
desk) and `rpidash3` (Pi 4, work desk). Per-device config lives in `deploy/hosts/<host>.env`;
Redis passwords come from `/etc/khomelab/secrets.env`, which k-homelab renders per host
from the age store (sprint 037) — this repo neither writes nor holds them; `KVSCF_TOKEN` is
still hand-installed to `/etc/kdeskdash/secrets.env` and never committed. The
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
just publish-publisher        # publish the claude-feed publisher bundle (its own version clock)
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
ctest --test-dir build --output-on-failure --no-tests=error
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
  `src/modeset.c`, `src/iconset.c`, `src/kvscf_feed.c`, `src/dev_telemetry.c`, `src/modes/claude_view.c`,
  `src/telemetry_host.c`, `src/bmp_write.c`, `src/clock_core.c`, `src/service_card.c`,
  `src/quickswitch.c`, `src/panel_state.c`, `src/panel_cmd.c`,
  `src/modes/dev_hostlist.c`, `src/modes/dev_view.c`) — no LVGL, no Redis, deterministic (RNG threaded through an
  explicit `uint32_t *state` seam). Each has a `tests/test_*.c`.
- **Modes** (`src/modes/*.c`) — each implements the `kd_mode_t` lifecycle from `src/mode.h`:
  `activate` / `deactivate` / `tick`, owning one LVGL screen and its private `state`. A mode
  does no ongoing work while deactivated. `*_mode_create(id, title)` builds and returns one.
- **Shared widgets** (`src/clock_widget.c` so far) — LVGL glue that is *not* a mode: takes a
  parent container, sizes itself to it, renders a pure core. The dual clock is one widget used
  by the Launcher's side pane and, since sprint 036 (WI #1136), the rebuilt `clock` mode.
  Reach for this shape
  when two modes want the same thing on screen — not by generalizing an existing full-screen
  mode, which is how you get a widget shaped like whichever mode happened to be first.
- **Shell** (`src/shell.c`, `src/shell.h`) — owns the set of modes, the active mode, and
  gesture navigation: swipe left/right cycles content modes (wrapping), swipe down opens the
  Menu, and a **double-tap on the bare background** jumps to the current mode's quick-switch
  partner (`src/quickswitch.c` — `KDESKDASH_QUICK_PAIRS` pins a partner, unset means the
  previously active mode, `none` disables it; the menu is excluded from that history).
  LVGL does not bubble `CLICKED` to a parent, which is what keeps the double-tap off calc
  keys and launcher buttons. It does **not** own mode storage; `main.c` keeps registered modes alive for the
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

#### Five independent feed handles — do not conflate them

Each has its own connection and failure isolation (a down endpoint never stalls boot or
another path). Three are `redis_client_t`s — the generic client + backoff lives in
`src/redis.c` / `redis_internal.h`, and each feed is a thin reader on its own handle. **Two
are libkdash `kdash_conn_t`s**: the claude feed since sprint 034, owned by its mode rather
than by a module main.c initialises, and the command feed since sprint 039.

1. **Commands from central** (`src/panel_feed.c`, `KDESKDASH_CMD_REDIS_*`) — read-only
   `kdash:panelmode:<host>` / `kdash:panelshot:<host>` on rpi53, polled ~1×/sec from the
   main loop. **Not a `redis_client_t`**: a libkdash handle on `KDASH_STEM_CENTRAL`. Acts on
   `ts` **advancing**, one acted stamp per verb, never clearing a key — so a mode picked by
   hand sticks. The panel's policy (which `{host}` it answers to, which screenshot paths it
   will accept) is pure and lives in `src/panel_cmd.c`. This replaced the **control**
   handle, a `redis_client_t` on the board's OWN Redis that carried remote mode control,
   last-mode persistence, GoL/GoLZ injection and the screenshot trigger.

   Durable state went the other way: it is a **file** now (`src/panel_state.c` pure core,
   `src/panel_store.c` store), not a feed at all. `KDESKDASH_REDIS_*` still names the local
   Redis, but only `panel_store.c`'s one-time migration reads it — on a first run with no
   state file, copying the old values across. Copy, never move.
2. **Telemetry** (`src/telemetry.c`, `KDESKDASH_TELEMETRY_REDIS_*`) — read-only kpidash host
   metrics for Dev mode. Defaults to host `rpi53`.
3. **Claude feed** (`src/modes/claude.c`, `KDESKDASH_CLAUDE_REDIS_*`) — fleet Claude Code
   agent activity + usage limits, fed by `publisher/claude-pub.sh` hooks. **Not a
   `redis_client_t`**: sprint 034 retired this panel's own `claude_feed.c`/`claude_redis.c`
   pair for libkdash's typed readers (`kdash_claude_sessions/_limits/_recent`), so the
   contract — key grammar, hash parsing, the display ladder, the attention-first sort,
   limits staleness — lives in `lib/kdashdata` and is shared with kstudiodash. The handle is
   opened on `KDASH_STEM_CLAUDE` (kdashdata CD-7: its own stem, never the one a kpidash
   reader uses, even though both answer `rpi53:6379`), and the mode owns it — so the "only
   dial an endpoint a registered mode uses" gate is structural rather than a roster in
   `main.c`. Two traps when touching it: `kdash_claude_sessions()` is a **counted reader**,
   so a negative return means "the read did not complete" and neither `out` nor `*skipped`
   carries information — never a count; and libkdash reads `$REDISCLI_AUTH` when `auth` is
   NULL, where this project means "no AUTH", so `claude.c` passes `""`. Panel-only display
   strings stay in `src/modes/claude_view.c` (CD-10).
4. **kvscf feed** (`src/kvscf_redis.c`) — one handle, **two** readers: the `foreground`
   ("Remote") mode reads `kvscf:instances:*` / `kvscf:edge:*` / `kvscf:apps:*`, and `launcher`
   reads `kvscf:launcher:*`. Both **publish** to `kvscf:focus:<host>` (`{id}`, `{app}` or
   `{button}` — kvscf's precedence is `button` > `app` > `id`). Its own handle *and* its own
   endpoint config (`KDESKDASH_KVSCF_REDIS_*`), each field falling back independently to the
   Claude feed's — a legacy of the days both lived on rpidash2:6380. Today both panels read
   the Claude feed from rpi53 and pin kvscf to `127.0.0.1:6380`, **their own board's second
   Redis instance**: rpidash2's `redis-claude` (cleo publishes to it; the name is historical)
   and rpidash3's `redis-kvscf` (kwork's). Both instances require AUTH and listen on loopback
   + the LAN address only — rpidash3's since sprint 026, rpidash2's since sprint 035 (korg WI
   2216 closed the tailnet-reachable unauthenticated listener kmon's nightly reported) — with
   the `requirepass` in a hand-installed `/etc/redis/redis-*-local.conf` that the committed
   conf `include`s, so a missing local file fails to start rather than starting open. So
   both gates are live on both boards, and they fail in opposite ways — a bad
   kvscf Redis password looks like an unreachable endpoint, a bad `KVSCF_TOKEN`
   looks like nothing at all. See `docs/kwork-rpidash3-pairing.md` and
   `deploy/hosts/README.md`. These are the only modes
   that **write/act on another machine**, gated by `KVSCF_TOKEN` (byte-exact, trimmed, never
   logged; per-kvscf-instance, so it stays in each device's hand-installed `secrets.env` —
   the one credential the fleet file does not carry, pending korg WI 2479). PUBLISH rides the
   ordinary command connection — kdeskdash never SUBSCRIBEs.

5. **Service card** (`src/service_pub.c`, `KDESKDASH_CARD_REDIS_*`) — **write-only**, and the
   only handle that is *not* mode-gated. Publishes this instance's own liveness to
   `kpidash:services:deskdash:<host>` every 15 s (no TTL — the kpidash board computes
   freshness from the payload's `ts` and reddens the card after 60 s) so every panel appears
   on the board. kdeskdash never *reads* that namespace; the contract lives in the pure
   `src/service_card.c` and is host-tested.

   **Why this is not the telemetry handle, although both reach `rpi53:6379`.** Telemetry is
   initialised only when Dev mode is registered, and the card must publish from *every*
   panel — sharing the handle would tie a panel's presence on the board to whether it
   happens to carry Dev. So the card gets its own handle, with its own endpoint config
   falling back to `KDESKDASH_TELEMETRY_REDIS_*` field by field and auth inherited **only
   when host and port both match** (the sprint-031 rule — see the long comment in
   `config.c`). Both Pis already point telemetry at `rpi53:6379`, so the card authenticates
   with no new env line on either device. If you find yourself merging these two because
   "they go to the same place", this paragraph is the reason not to.

Feeds are initialised **only for modes the modeset registered**, so a panel without Dev never
dials the telemetry endpoint at all. A handle shared by two modes is initialised when *either*
is registered — see the kvscf gate in `main.c`. **Two are deliberate exceptions**, and for
the same reason: they address the *instance*, not a mode. The service card reports that this
panel is alive whatever it carries; the command feed is how the panel is told what to show,
and gating "can this panel be commanded" on "does it happen to carry some particular mode"
would be an odd appliance. Both are initialised unconditionally.

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
- **A fresh mtime is not a finished file** (`a-fresh-mtime-is-not-a-finished-file.md`) —
  a writer publishes by `rename()`ing a complete file into place: `<path>.tmp` in the **same
  directory**, `fsync`, then rename, and on failure unlink the temp and leave the target
  alone. `fopen(path,"wb")` sets a fresh mtime on the first byte, so a reader waiting for
  freshness reads a prefix — WI 2308's truncated screenshots. Fix it on the **writer**, which
  fixes every consumer at once and lets the readers get simpler. Same shape in
  `panel_state_save()`, where a torn file after a power cut is the failure. The test that
  catches a regression is that a **failed** write leaves the previous target byte-identical.
- **Sandboxing needs a second device** (`systemd-sandboxing-needs-a-second-device.md`) —
  `install-service` never overwrites a device's env file but *does* overwrite the unit, so a
  fleet drifts one device at a time. `PrivateTmp=yes` had been hiding device screenshots
  since sprint 010 and only surfaced when rpidash3 became the first host to run the committed
  unit. Re-run `install-service` everywhere after touching `deploy/kdeskdash.service`, and
  prefer `StateDirectory` for anything the outside world must read.
- **Proportional digits move their neighbours** (`proportional-digits-move-their-neighbours.md`)
  — no font in this build has tabular digits, so a label whose text ticks changes width, and
  in a centered `LV_SIZE_CONTENT` flex row it hands half of that change to its *sibling* as
  movement. Pin the volatile label to its widest rendering and align the text toward the
  stable element. `lv_obj_align` has the same failure one anchor away (clock mode's stopwatch
  did, at 10 Hz, until sprint 036's rebuild pinned it). Check every live readout for which
  edge the digits push — and where the readout's *length* changes and not just its glyphs,
  the pinned width has to track the current shape rather than one fixed maximum.
- **Tap and hold on one widget** (`lvgl-tap-and-hold-on-one-widget.md`) — LVGL sends
  `CLICKED` on release *whether or not* `LONG_PRESSED` already fired, so a tap/hold pair
  wired to `CLICKED` has the tap undo the hold every time, silently. `SHORT_CLICKED` is the
  one gated on `long_pr_sent == 0`. The swipe guard does cover a hold (`indev_gesture()`
  runs earlier in the same press iteration), and a target carrying two gestures is sized
  for the one you cannot afford to miss — a missed hold falls through to the tap.
- **Adopting a library inherits its defaults** (`adopting-a-library-inherits-its-defaults.md`)
  — a library's "unset" is not yours. Swapping `claude_redis.c` for libkdash forwarded the
  same nullable `auth` to a field where `NULL` means *read `$REDISCLI_AUTH`*, not *no AUTH*,
  which would have had a panel authenticate with the control Redis's password on a refactor
  that changed nothing else. Read the new type's docs for every parameter you forward, not
  just the ones you edit — especially where a sentinel (`NULL`, `0`, `""`, `-1`) carries
  meaning. Same sprint, same shape: `kdash_claude_sessions()`'s negative return is "the read
  did not complete", never a count.
- **Verify the side that actually connects** (`verify-the-side-that-actually-connects.md`) —
  a plan asserting "both sides already support X" is a claim about two specific binaries;
  enumerate them and read the source. Sprint 026's said so about kdeskdash's reader and
  rpi53's server, neither of which was in the path — the program that had to authenticate was
  kvscf, which could not. Corollaries: transport auth (`*_REDISCLI_AUTH`) and application
  auth (`KVSCF_TOKEN`) fail in *opposite* ways, so "data is flowing" tests only one of them;
  and a conf whose insecure default must never start should `include` an uncommitted file, so
  a missing secret fails closed and loudly.

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
  **The migration is done** (sprint 033, korg WI 514): there is no bare `lv_color_hex(0x…)`
  left in `src/`, and a mode's local `COLOR_*` names are aliases defined as `PAL(NAME)`.
  Adding one back is a regression, not a shortcut. From inside `src/modes/`, include the
  core header as `"../palette.h"` — `src/modes/palette.h` is the *mode* header and shadows
  it (`docs/solutions/best-practices/quote-include-core-header-shadowing.md`).
- LVGL is a pinned submodule at `lib/lvgl` (v9.2.2); libkdash (kdashdata) is a pinned
  submodule at `lib/kdashdata`; cJSON is vendored at `lib/cjson` (and again inside
  kdashdata, byte-identical — the linker keeps one copy).
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
