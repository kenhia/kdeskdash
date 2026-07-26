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
just deploy [host]            # cross-compile, scp, restart the service (default rpidash2)
just sync-sysroot [host]      # one-time / after Pi apt changes: rsync a Pi sysroot to ~/pi-sysroot
just install-service [host]   # one-time per device: systemd unit + that host's env file
just golz-mc --help           # headless GoLZ balance sweep (Monte Carlo over the pure core)
```

The underlying commands, when you need them directly:

```bash
cmake -B build && cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
cmake -B build-pi -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake
cmake --build build-pi --target kdeskdash -j"$(nproc)"
```

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
  `src/iconset.c`, `src/kvscf_feed.c`, `src/dev_telemetry.c`, `src/claude_feed.c`,
  `src/telemetry_host.c`, `src/bmp_write.c`, `src/modes/dev_hostlist.c`,
  `src/modes/dev_view.c`) — no LVGL, no Redis, deterministic (RNG threaded through an
  explicit `uint32_t *state` seam). Each has a `tests/test_*.c`.
- **Modes** (`src/modes/*.c`) — each implements the `kd_mode_t` lifecycle from `src/mode.h`:
  `activate` / `deactivate` / `tick`, owning one LVGL screen and its private `state`. A mode
  does no ongoing work while deactivated. `*_mode_create(id, title)` builds and returns one.
- **Shell** (`src/shell.c`, `src/shell.h`) — owns the set of modes, the active mode, and
  gesture navigation: swipe left/right cycles content modes (wrapping), swipe down opens the
  Menu. It does **not** own mode storage; `main.c` keeps registered modes alive for the
  program's lifetime. A change callback (`shell_set_change_cb`) persists the active mode to Redis.
- **Entry** (`src/main.c`) — DRM display + evdev touch bring-up, registers every mode, wires
  the three Redis handles, runs the LVGL main loop until SIGINT/SIGTERM, tears down cleanly.

**Adding a mode is one registration call** in `main.c` (`shell_register_content_mode(...)`),
plus the mode's `.c`/`.h` and its source line in `CMakeLists.txt`.

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
4. **kvscf feed** (`src/kvscf_redis.c`) — the `foreground` ("Remote") mode: reads
   `kvscf:instances:*` and **publishes** `kvscf:focus:<host>` on the *same 6380 instance* as
   the Claude feed, but on its own handle (reuses the `KDESKDASH_CLAUDE_REDIS_*` endpoint
   config; own connection for isolation). This is the only mode that **writes/acts on another
   machine** (foregrounds a window), gated by the shared `KVSCF_TOKEN` (byte-exact, trimmed,
   never logged). PUBLISH rides the ordinary command connection — kdeskdash never SUBSCRIBEs.

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
  harness and is the most recent. New work gets a new
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
