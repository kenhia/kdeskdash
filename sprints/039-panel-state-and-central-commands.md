# Sprint 039 — Panel state in a file, panel commands from central

korg proposal **2933**, slice 3 of program **2935** (“Two Redis servers”).
Covers **WI 2308** (kddss cats a half-written screenshot BMP).
Overseen leg `kdeskdash-829b01`; the ship is gated on the overseer’s green
light.

## Goal

Stop kdeskdash needing a Redis server on its own board.

Two halves, and they are independent:

1. **Durable state → one file.** The GoLZ counters and adaptive threshold, the
   calculator registers, the dev-mode host assignments and the last active mode
   move from `kdeskdash:*` on the loopback Redis to `/var/lib/kdeskdash/state`.
   Atomic write (temp + rename); a malformed file is rejected whole, the way
   `calc:regs` already is. One-time migration copies the old values across on
   first run — copy, never move.
2. **Commands → central, read-only.** Mode switch, settings injection and the
   screenshot trigger come from the kdashdata control families on rpi53
   (`kdash:panelmode:{host}`, `kdash:panelshot:{host}`, contract slice 2931 /
   CD-22), acted on when `ts` advances. The panel never clears a command.

## Premise check (start-sprint Step 5)

| claim | verdict |
|---|---|
| **WI 2308**: `scripts/kddss` cats the BMP as soon as its mtime goes fresh | **holds** — `fresh()` tests `stat -c %Y` only, then `cat`s |
| **WI 2308**: the device writes the BMP in place, so a fresh mtime ≠ a complete file | **holds** — `screenshot.c` `fopen(path,"wb")` straight onto the target |
| **WI 2308**: `bmp_write.c` is a pure host-tested core, so the rename is one call in the caller | **holds** — `tests/test_bmp_write.c` is registered |
| Proposal: durable panel data lives on the loopback Redis | **holds** — `redis.h` schema block; golz counters, `calc:regs`, `dev:left/right`, `active_mode` |
| Proposal: settings arrive as `HGETALL`+`DEL`, screenshot as `GETDEL` | **holds** — `redis_apply_gol_settings` / `redis_apply_golz_settings` / `redis_poll` |
| Proposal: kdeskdash already links libkdash, so the readers are available on a submodule bump | **holds** — pinned at `bb29b69` (sprint 009); `kdash_panelmode`/`kdash_panelshot`/`kdash_cmd_actionable` are on kdashdata `origin/main` at `8683ce1` (sprint 014) |

No cross-project plan applies: `kdeskdash` is not in
`cross-project-planning/index.md`.

Per overseer comment 2659 the slice order changed — this runs **before** 2932,
and `config.c`’s kvscf endpoint resolution is 2932’s and is left alone here.

## Decisions

### The state file is a flat `key=value` text file, not JSON

`/var/lib/kdeskdash/state`. cJSON is vendored and libkdash parses JSON, so JSON
was available — but nothing reads this file except the panel that wrote it, and
a flat file keeps the core stdlib-only and the failure modes countable. It
matches the “less framework” grain of the rest of the pure cores.

**Whole-file rejection, with one deliberate exception.** A line that is not
blank, not a `#` comment and not `key=value` rejects the file entirely, as does
a value a known key cannot parse. An **unknown key** is ignored and counted,
because naming an older version *is* the rollback here (`just deploy <old>`):
a key added in a later version must not brick the state file on the way back
down.

### `{host}` is the lowercase short hostname

`kdash:panelmode:{host}` needs the name the panel answers to on central.
`gethostname()` on rpidash2 returns **`rpiDash2`** (korg WI 2277 — it is why
that board’s service-card key is mixed-case), so the panel lowercases the first
label and validates it against the kdashdata token charset:
`rpiDash2.local` → `rpidash2`. `KDESKDASH_PANEL_HOST` overrides it.
**Used here: the lowercase inventory names `rpidash2` and `rpidash3`.**

### A screenshot path from the wire is refused outside `/var/lib/kdeskdash/`

`kdash:panelshot:{host}`’s `path` is absolute by contract, and writable by every
holder of the central password (CD-23), so the directory policy is the panel’s.
`panel_cmd_shot_path_ok()` requires the `/var/lib/kdeskdash/` prefix, a
non-empty remainder and no `..` segment; a refusal is logged with the path.
Negative-tested with paths outside the directory and with traversal.

### Each verb keeps its own acted stamp

`kdash_cmd_actionable()` per family, as `kdash_feed.h` requires — a screenshot
and a mode switch are separate edges. Sharing one stamp would make acting on
either suppress the next of the other kind.

### Settings still consume once, per mode

The old feed had two keys (`gol:settings`, `golz:settings`) and each
`HGETALL`+`DEL` consumed its own. The contract carries one `settings` object,
so the port keeps **two consume flags over one pending set**: GoLZ’s reseed
reads both vocabularies in sequence and must not have the first consume the
second. Behaviour is byte-for-byte the old one-shot.

### WI 2308 is fixed device-side

`bmp_write_file_atomic()` writes `<path>.tmp` and `rename()`s it, so the target
only ever exists complete and *every* consumer is fixed — `kddss`, and the
`deploy-panels` skill’s `kddss deploy-$V` step where it bit during the
sprint-035 ship. `kddss` therefore keeps no size-stability poll of its own.

## What shipped

**Two pure cores, two thin modules, and the control handle gone.**

| new | what |
|---|---|
| `src/panel_state.c/.h` | The state file: parse, serialize, load, atomic save. Pure stdlib. `tests/test_panel_state.c`. |
| `src/panel_cmd.c/.h` | Central-command policy: the `{host}` this panel answers to, and the screenshot path guard. Pure. `tests/test_panel_cmd.c`. |
| `src/panel_store.c/.h` | The write-through store over the file, plus the one-time Redis migration. |
| `src/panel_feed.c/.h` | libkdash glue for `kdash:panelmode` / `kdash:panelshot`. |

`src/redis.c`/`.h` lost the whole control client and kept only the generic
`redis_client_t` the four remaining handles share. `redis_golz_incr_wins()` went
with it — declared since the machete era, called by nothing.

`golz_settings_apply_field()` moved out of `redis.c` into `gol_settings.c`
beside its Conway sibling, because the settings injection had to move anyway.
It parsed with `atoi()` there, which coerces `"abc"` to `0` — and `0` is **in
range** for `initial_count`, `machete_percentage` and `human_kill_zombie`, so a
junk value silently zeroed them. It gets the strict parse now, and
`tests/test_settings.c` covers both appliers including each ignoring the
other’s field names (they share one `settings` object on the wire).

`screenshot.c` writes through `bmp_write_file_atomic()`. `scripts/kddss`
publishes `kdash-pub set kdash:panelshot:<host>` from the local machine and
waits for the panel’s file mtime to *change* — compared against itself, so no
clock of this host’s is compared with the panel’s. Its `/tmp` fallback is gone:
a panel old enough to write `/tmp` does not answer the central key at all.

`lib/kdashdata` bumped `bb29b69` → `8683ce1` (kdashdata sprint 014, the merged
contract).

Docs: the README’s “Redis (optional)” section is now **Remote control (from
central)** + **Durable state (one file)**; `CLAUDE.md`’s feed-handle list,
pure-core list and mode-gate exceptions; both host env files;
`deploy/kdeskdash.env.example`; the unit; and the `deploy-panels` skill’s
`kddss` step, which now needs `kdash-pub` on the deploying host.

### The unit changed — `install-service` must be re-run on BOTH devices

`After=redis-server.service` stays (ordering for the migration window);
`Wants=` is **gone**, because nothing here should pull a Redis server in, and
`After=` on a unit that does not exist is silent — so slice 2934 removes the
servers with no edit needed back here. `StateDirectory`’s comment now names all
three things that live in it and ties the directory to the path guard.

A deploy does **not** update the unit
(`docs/solutions/best-practices/systemd-sandboxing-needs-a-second-device.md`),
so the ship must run `just install-service rpidash2` **and**
`just install-service rpidash3`, or the fleet drifts one device at a time.

## Repaired in passing

- **`golz_settings_apply_field`’s `atoi()`** — described above. Inside the
  change rather than beside it: the function had to move, and moving it onto
  the strict parse its sibling already used was one line per field.
- **A stale claim in a lesson doc.** `systemd-sandboxing-needs-a-second-device.md`
  said `kddss` falls back to `/tmp`. It no longer does; the paragraph now says
  why removing the fallback is the rule rather than an exception to it.
- **`redis_golz_incr_wins()`**, dead since the machete counters replaced it.

## Gates

`just check` — build + `unit-lint` + **24** ctest suites, all green.
`cmake --build build-pi --target kdeskdash` — the aarch64 cross-build links
clean, no warnings.

**Negative-tested**, four planted faults, each caught by the test written for it:

| planted | caught by |
|---|---|
| the parser builds into the caller’s struct, so a late rejection keeps early fields | “and the fields read before it are NOT kept (whole-file rule)” |
| the path guard stops rejecting a `..` segment | “`..` traversal”, “nested `..` traversal”, “bare `..`” |
| the host segment is no longer lowercased | `host("rpiDash2") -> "rpidash2"` (+3 more) |
| the screenshot writes straight to the target, no temp + rename | “the previous target survives a failed write” |

## Live acceptance — fired, not deferred

Run **from kai**, which is the host that runs `kdash-pub` and `kddss` and
therefore the host whose reachability is the one that matters.

`kdash_dump` built from **this repo’s pinned submodule** and pointed at the real
central Redis, after publishing both commands with `kdash-pub`:

```
panelmode: mode=golz  3 setting(s)  issued 10s ago  [a booting panel would act on this]
             density = 0.35
             machete_percentage = 12
             speed_ms = 40
panelshot: path=/var/lib/kdeskdash/kdash-039-check.bmp  issued 10s ago  [...]
panel:     no command
```

That proves the submodule pin carries the readers, the endpoint resolves, the
fleet password works, `kdash-pub` stamps `ts`, the edge rule agrees, and the
three families are read independently. The settings deliberately mixed both
vocabularies — `density`/`speed_ms` are Conway, `machete_percentage` is GoLZ —
which is the case the two-appliers-over-one-object design exists for.

Then, for the guard: publishing `{"path":"/etc/cron.d/kdeskdash"}` is **accepted
by the contract** (it is absolute) and **refused by the panel** — which is the
point of the policy living here rather than in the schema.

Host `kai` was used because nothing consumes kai’s panel keys. Both probe keys
were deleted afterwards and the deletion verified: these families are ts-owned
with no TTL, so a probe left behind would sit on central forever.

**Still owed at ship time, and it is a trigger rather than a soak:** five
consecutive good `kddss` shots against a deployed panel (overseer comment 2653).
It needs the build on a board, which is the deploy step of the ship — not a
`check_after` date.


## Deployed

**`0.27.0-13e01e4`** (squash `13e01e4`, PR #46) published to the package store
and installed on **both** panels, 2026-09-20, via the `deploy-panels` skill
declared in `.sprint-deploy`. Run from kai, the only host that can deploy — the
Pis are unmanaged and hold no store credentials.

**`just install-service` on both boards**, because this sprint changed
`deploy/kdeskdash.service`. Both kept their existing
`/etc/kdeskdash/kdeskdash.env` (`install-service` never overwrites it), which is
correct here: the env-file edits were comments plus the new command-feed note,
and **no device needs a new variable** — the command feed falls back to the
telemetry endpoint, which is already `rpi53:6379` on both.

| | rpidash2 | rpidash3 |
|---|---|---|
| `--version` | `kdeskdash 0.27.0-13e01e4` | `kdeskdash 0.27.0-13e01e4` |
| unit | active | active |
| `Wants=` redis | *(none — the removal landed)* | *(none)* |
| `After=` redis | `redis-server.service` | `redis-server.service` |
| `{host}` chosen | `rpidash2` | `rpidash3` |

### The four proofs, fired the same day

All four are **triggers**, not soaks — the cleared sequence in handoff
korg:2950 — and all four passed.

**1. Migration.** Each panel wrote `/var/lib/kdeskdash/state` on first start and
its contents equal the Redis values it copied from, field for field:

| | rpidash2 state file | rpidash2 Redis |
|---|---|---|
| `golz.human_wins` | 1347 | 1347 |
| `golz.zombie_wins` | 1343 | 1343 |
| `golz.ties` | 3871 | 3871 |
| `golz.gens_to_win` | 262 | 262 |
| `golz.wins` | 13883 | 13883 |
| `dev.left` / `dev.right` | `kai` / `kubs0` | `kai` / `kubs0` |
| `active_mode` | `golz` | `golz` |

**The Redis keys are all still there** — copy, never move, so a rollback finds
its state where it left it. rpidash3 carries no GoLZ keys (its mode set omits
the simulations), and its three keys copied exactly: `dev.left=kai`,
`dev.right=kubs0`, `active_mode=launcher`.

*(The GoLZ counters moved between the pre-deploy capture and the copy —
`zombie_wins` 1342→1343, `gens_to_win` 265→262 — because the panel kept playing
right up to the restart. The proof is that the file equals what it copied, and
it does. From here the file is the authority and the two will diverge, which is
the point.)*

**The `{host}` question, settled live in one journal line:**

```
Sep 20 20:09:47 rpiDash2 kdeskdash[8586]: kdeskdash: migrated panel state from 127.0.0.1:6379 (copied, not moved)
Sep 20 20:09:47 rpiDash2 kdeskdash[8586]: kdeskdash: central commands as "rpidash2" (rpi53:6379)
```

systemd's own prefix says `rpiDash2` and the panel says `rpidash2` — korg
WI 2277 and the reason the name is lowercased, in adjacent columns of one line.

**2. Mode command, and put back.** rpidash2 was on `golz`.
`kdash-pub set kdash:panelmode:rpidash2 '{"mode":"clock"}'` switched it within
one poll; `.scratch/039/mode-proof-clock.png` is the clock face on the panel,
captured through the rewritten `kddss`. The state file read `active_mode=clock`,
which proves the switch *and* the change callback persisting it.
**Restored to `golz`** the same way and confirmed. **rpidash3's screen was not
touched** — it is Ken's work desk, and its journal line plus state file are
proof enough there.

**3. WI 2308 — five consecutive `kddss` shots from rpidash2, all complete.**
Each opened and `load()`ed with PIL, which is the exact check that rejected 3 of
5 in sprint 035:

```
  shot 1: OK   1920x440  RGB
  shot 2: OK   1920x440  RGB
  shot 3: OK   1920x440  RGB
  shot 4: OK   1920x440  RGB
  shot 5: OK   1920x440  RGB
```

No `.tmp` debris left in `/var/lib/kdeskdash/` afterwards.

**4. Path guard, live.** One command naming a path outside the state directory —
and deliberately one the service *could* write, since `PrivateTmp=yes` gives it
a writable `/tmp`, so only the guard stops it:

```
Sep 20 20:11:18 rpiDash2 kdeskdash[8586]: kdeskdash: refusing screenshot path "/tmp/kdeskdash-guard-probe.bmp" — outside /var/lib/kdeskdash/
```

The file was never created, and `kdeskdash-shot.bmp`'s mtime was **unchanged** —
a refusal is a refusal, not a silent fall back to the default.

**Probe keys deleted and the deletion verified**: `kdash:panelmode:*` and
`kdash:panelshot:*` for `rpidash2`, `rpidash3` and `kai` all return `0` from
`EXISTS` on central. These families are ts-owned with no TTL, so one left behind
would sit there forever.
