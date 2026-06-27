---
title: "feat: GoLZ machetes — give the Humans a fighting chance"
type: feat
status: completed
date: 2026-06-27
origin: conversation (friend request — "machetes")
builds_on: docs/plans/2026-06-09-002-feat-golz-zombies-mode-plan.md
---

# feat: GoLZ machetes — give the Humans a fighting chance

## Overview

In GoLZ the Zombies almost always win; the only Human "strategy" is to stabilise
into a still life and hope no Zombie wanders by. This change arms the Humans with
**machetes**: a third static layer, seeded once per round, that lets a living cell
standing on a machete kill one nearby Zombie each generation. It also introduces a
real **Human win condition** (outlast the Zombies for `generations_to_win`
generations), a **Tie** outcome (the board stabilised — neither side resolved),
and a self-balancing difficulty knob that drifts toward a ~50/50 match over time.

The work continues the established two-layer pattern (see
[two-layer-faction-reuse.md](../solutions/best-practices/two-layer-faction-reuse.md)):
`gol_t`/`gol_step` stay byte-for-byte unchanged, the new machete behaviour is one
more parallel grid plus one new ordered phase inside `golz_step`, and everything
stays pure and host-testable. Starting parameter ranges are chosen by a headless
**monte-carlo** sweep rather than guessed.

## Problem Frame

The audience is the unattended panel viewer; the mode must run forever and never
freeze. Today's terminal states are only *Zombie win* and *quiet restart*. Players
have no way to "win", and the match is lopsided. We want:

1. A Human edge (machetes) that is tunable and visible.
2. A decisive, bounded Human win path.
3. Scored outcomes for all three results: **Zombies / Humans / Ties**.
4. A starting balance near 50/50, found empirically, that then self-corrects.

## Design decisions (from the requester)

- **Humans win only by reaching `generations_to_win`** with at least one living
  cell. Eradicating every Zombie is *not* an instant win — Zombies respawn from
  the dead (and reinfect the eaten), so "no Zombies right now" is not safety. Many
  observed Zombie wins start from a board with zero Zombies.
- **A stabilised living board (the existing living-only cycle/extinction rule) is a
  Tie**, not a non-result. Ties are counted.
- **Three counters**: Zombies, Humans, Ties.
- **`generations_to_win` self-balances**: `+3` after a Human win, `-5` after a
  Zombie win, **floor 100**. Ties do not move it. Starting value (WMC) 250.
- **Chaos seeding for machetes**: roll `machete_percentage` independently per cell,
  so the actual machete count varies widely around the nominal rate.
- **Machete reach**: a living cell at `(x,y)` on a machete may kill one Zombie
  chosen at random from any within Chebyshev distance 2 — `x±{0,1,2}, y±{0,1,2}`
  (the 5×5 block minus the centre), toroidally wrapped — at probability
  `human_kill_zombie` (only rolled when a Zombie is actually in range).

## Requirements Trace (M-series, extends R1–R23)

- M1. New static layer `machetes` (0/1, `cols*rows`), seeded once per round; never
  born, never dies, never moves.
- M2. Seeding: for each cell, independent roll `< machete_percentage` → machete.
  When `machete_percentage == 0`, draw **no** RNG (preserves existing streams).
- M3. New ordered phase **machete turn**, between the Living turn and the Zombie
  turn (movement). Humans strike first.
- M4. Per machete-occupied living cell, in reshuffled order: gather live Zombies in
  the 5×5 (Chebyshev≤2) toroidal neighbourhood; if ≥1, roll `human_kill_zombie`;
  on success kill one chosen at random. Kills read/write the live zombie grid
  (no double-kill: a Zombie already killed this turn is not a candidate).
- M5. The machete turn never touches living cells, `died_mask`, or spawn
  bookkeeping; killed Zombies fade via the normal red trail.
- M6. New settings: `machete_percentage` [0,100], `human_kill_zombie` [0,100],
  `generations_to_win` (≤0 = disabled; game-rule floor 100 enforced by the mode).
- M7. New terminal outcome **`GOLZ_HUMAN_WIN`**: `generations_to_win > 0` and
  living `> 0` and `generation >= generations_to_win`.
- M8. Rename **`GOLZ_QUIET_RESTART` → `GOLZ_TIE`** (same trigger: living-only
  cycle/extinction, or the max-generation backstop). Semantics now "scored tie".
- M9. Terminal precedence: Zombie win (living==0 && zombies>0) → Human win (M7) →
  Tie (cycle/extinction or backstop) → continue.
- M10. Render: a cell with **both** a living cell and a machete is **blue**
  (`0x0000FF`) instead of green; its fade trail is blue too (machetes are static,
  so a trail cell is blue iff that cell holds a machete). Zombie red unchanged;
  red+blue compose like red+green did.
- M11. Redis: post-machete counters `human_wins`, `zombie_wins`, `ties` (all start
  at 0); persistent adaptive `gens_to_win` (default 250, floor 100); legacy
  `kdeskdash:golz:wins` (~13,883) retained read-only as **historical** Zombie wins.
- M12. End-of-game banner shows the winner (Humans / Zombies / Tie), all three
  counts, the next round's `generations_to_win`, and (smaller) the historical
  Zombie wins.
- M13. Per-round `machete_percentage` and `human_kill_zombie` are rolled from the
  monte-carlo-determined ranges; `generations_to_win` comes from Redis (adaptive),
  not rolled. Both new fields are injectable via the Redis settings hash.
- M14. Monte-carlo harness: headless sweep over the new knobs against the real
  randomized settings distribution; reports Human/Zombie/Tie rates; used to pick
  the M13 starting ranges.

## Architecture

### Pure core (`src/golz.c` / `src/golz.h`)

- `golz_t` gains `uint8_t *machetes`.
- `golz_settings_t` gains `int machete_percentage, human_kill_zombie,
  generations_to_win`.
- `golz_settings_clamp` clamps the two percentages to [0,100] and
  `generations_to_win` to `>= 0` (0 = disabled; the 100 floor is a game rule, not
  a core clamp, so tests can disable the threshold by leaving it 0).
- `golz_init` / `golz_free` / `golz_clear` / `golz_seed` allocate, free, zero, and
  (M2) roll machetes — rolled **last** in `golz_seed` and **skipped entirely** at
  `machete_percentage == 0` so all existing deterministic tests are unperturbed.
- New `gz_machete(g)` runs in `golz_step` between `gz_living_turn` and `gz_move`.
- `golz_compose_pixel` recolours living/trail to blue at machete cells (M10).
- `golz_terminal` gains M7/M8/M9. `gol_cycle_record` is still called every
  generation to keep the ring valid; its result now maps to `GOLZ_TIE`.

New step pipeline:

```
promote z_new → snapshot prev_living → reset died_mask → living turn (Conway) →
MACHETE TURN (new) → zombie movement → eat/kill+reinfect → spawn → ztrail → gen++
```

### Mode (`src/modes/golz.c`)

- `roll_settings` draws `machete_percentage` and `human_kill_zombie` from the
  monte-carlo ranges (still **last** in the stream so existing draws don't shift),
  and sets `generations_to_win` from the adaptive Redis value (default 250).
- New `GOLZ_PHASE_END` (or reuse banner) renders the three-outcome banner (M12).
  Zombie win keeps its downward victory lap; Human win and Tie go straight to the
  banner.
- On each decisive outcome: increment the matching counter, and for Human/Zombie
  wins adjust + persist `gens_to_win` (`+3` / `-5`, floor 100).
- Render path recolours blue cells via the updated `golz_compose_pixel`.

### Redis (`src/redis.c` / `src/redis.h`)

- New keys: `kdeskdash:golz:human_wins`, `kdeskdash:golz:zombie_wins`,
  `kdeskdash:golz:ties`, `kdeskdash:golz:gens_to_win`.
- Helpers: `incr`/`get` for the three counters; `get`/`set` for `gens_to_win`
  (with the 100 floor); reuse `redis_golz_get_wins` for the historical value.
- `apply_golz_field` learns `machete_percentage`, `human_kill_zombie`,
  `generations_to_win` for one-shot injection.

### Monte-carlo (`tools/golz_mc.c`, CMake target `golz_mc`, not a ctest)

Links `src/golz.c` + `src/gol.c`. For each parameter combo, runs N games; each
game samples the real randomized settings distribution (cell_size 2–30, density
0.15–0.5, initial zombies 0–2, reinfect 0–100, spawn 0–40) against a representative
resolution (CLI arg, default chosen to mirror the panel), seeds, and steps until a
terminal outcome. Prints a Human/Zombie/Tie table to pick M13 ranges. Resolution
and trial count are CLI-tunable so the requester can re-run for their exact panel.

## Testing

Existing `tests/test_golz.c` stays green because new behaviour is gated on
`machete_percentage > 0` and `generations_to_win > 0` (both 0 in the current
`mk_cfg`). Terminal tests that referenced `GOLZ_QUIET_RESTART` are updated to
`GOLZ_TIE` (same triggers).

New tests:

- Machete seeding: `pct==0` → none and zero RNG draw (parity preserved); `pct==100`
  → all cells; a mid value places some.
- Machete kill: 2×2 living block on machetes + one Zombie in range,
  `human_kill_zombie==100` → Zombie dies during the machete turn (before it can
  eat); block survives. `human_kill_zombie==0` → Zombie lives.
- Reach boundary: a Zombie at Chebyshev distance 2 is killable; distance 3 is not.
- Compose: living+machete → blue; blue trail fades on the blue channel; red+blue
  composes.
- Terminal: Human win at the threshold; Tie on a still life; Zombie-win precedence
  unchanged.

## Monte-carlo results (1920×440 panel, real randomized settings)

Headline finding — **the +3/−5 threshold rule is itself the balancer, and its
equilibrium is not 50/50.** At a steady state the expected threshold change is
zero, so `3·P(Human) = 5·P(Zombie)` over all games, i.e. the decisive Human share
`H/(H+Z)` converges to **5/8 = 62.5%** *regardless of the machete parameters*. The
machete ranges therefore do **not** set the win ratio — they set the **tie rate**
and **where the threshold self-settles**. (Want a true 50/50 decisive match?
Use symmetric steps, e.g. +3/−3; the constants live in one place in the mode.)

Static-grid sweep (gens=250, 150 games/cell) confirmed the requester's WMC machete
guesses were far too generous: at machete ≥ 30% the decisive Human share is
70–100%. The ~50% contour sits low (machete ~10–20, kill ~25–35) but with ~60%
ties.

Adaptive end-to-end runs (1000 games, live +3/−5 from a 200 start) for candidate
ranges, back-half steady state:

| machete | kill  | eq. threshold | ties | H% / Z% | decisive H/(H+Z) |
|---------|-------|---------------|------|---------|------------------|
| 8–18    | 18–32 | ~170          | 47%  | 34 / 19 | 63%              |
| 10–22   | 20–35 | ~250          | 60%  | 27 / 14 | 66%              |
| **10–25** | **22–38** | **~250**  | 58%  | 27 / 16 | **63%**          |
| 12–25   | 25–40 | ~310          | 60%  | 25 / 14 | 65%              |

**Shipped (final):** symmetric steps **+3/−3** for a true 50/50 decisive match,
paired with `machete_percentage` **8–18**, `human_kill_zombie` **18–32**,
`generations_to_win` default **250** (floor 100). With +3/−3 the equilibrium
decisive share is 50%; the 8–18/18–32 band lands the self-balanced threshold at
~230 (near the 250 default, minimal drift) and keeps ties to ~59% — back-half
steady state H≈20% / Z≈21% / T≈59%, decisive ≈48–52%.

(An earlier draft shipped +3/−5 with machete 10–25/22–38, whose natural fixed
point is ~62.5% Human; switched to symmetric at the requester's call. Note that
under +3/−3 the same 10–25/22–38 band overshoots the threshold to ~390 and ties
to ~70%, which is why the band was lowered to 8–18/18–32 to re-pair with the
symmetric rule.) Reproduce or re-tune with `./build/golz_mc` (static sweep) and
`./build/golz_mc --adaptive --machete LO-HI --kill LO-HI --steps WIN/LOSS` (live
loop); `--res WxH` for a different panel.

Ties (~50–60%) are intrinsic: a stabilised living board trips the (retained)
living-only cycle rule, which the requester chose to score as a Tie. They are not
a bug — they are the old "quiet restart" outcome, now counted.

## Open items / assumptions

- Monte-carlo resolution defaults to a representative panel size; the requester can
  re-run with `--res WxH` for the exact device. Self-balancing `gens_to_win` makes
  the starting ranges non-critical anyway — it converges.
- Historical Zombie wins come from the legacy `kdeskdash:golz:wins` key (whatever
  it currently holds, ~13,883), displayed read-only; the new era starts all
  counters at 0.
