---
title: "Two-layer faction reuse: wrap an unmodified pure simulation core instead of generalizing it"
date: 2026-06-09
category: docs/solutions/best-practices
module: Game of Life / GoLZ simulation cores (src/gol.c, src/golz.c)
problem_type: best_practice
component: service_object
severity: medium
applies_when:
  - Building a new feature that is "the existing simulation, plus another interacting layer/faction"
  - The base core is well-tested and already shipping in a user-facing mode
  - The base step function reads one buffer and writes another (no in-place mutation mid-step)
  - You are tempted to widen the core's cell type (2-state -> 3-state) or add branches to its hot path
related_components:
  - game_of_life mode
  - golz mode
  - cycle detection
  - host test harness
tags: [architecture, reuse, simulation, pure-core, composition, lvgl, testability, determinism]
---

# Two-layer faction reuse: wrap an unmodified pure simulation core instead of generalizing it

## Context

GoLZ ("Game of Life with Zombies") needed a second, interacting faction — roaming
zombies that move, eat living cells, reinfect, and spawn — layered on top of the
existing Conway Game of Life. The obvious-but-wrong instinct is to **generalize the
core**: widen `gol_t`'s cell from a 2-state `uint8_t` to a 3-state enum (empty /
living / zombie) and teach `gol_step` about the new rules.

That instinct is expensive. `gol_step` and `gol_t` are the most-tested, highest-
blast-radius code in the project, and they already ship in the live Game of Life
content mode. Editing them to serve a *second* mode forks risk into the first mode,
balloons the change surface, and entangles two rule sets in one hot loop.

This doc captures the pattern we used instead, which kept `gol_t`/`gol_step`
byte-for-byte unchanged while still composing a rich second faction on top.

## Guidance

**Keep the existing pure core unmodified and embed it by value as one layer; build
the new layer as parallel grids in a new pure module that orchestrates the core.**

Concretely, for GoLZ:

- `golz_t` **embeds** `gol_t living` by value (the living faction *is* an ordinary
  Game of Life board) and adds parallel `uint8_t *` grids for the new faction:
  `zombies`, `z_new` (deferred-activation buffer), `z_trail` (fade), plus
  bookkeeping grids `prev_living`, `snapshot`, `died_mask`, and a reusable
  `int *empties` scratch buffer.
- `golz_step` calls `gol_step(&g->living)` **verbatim** for the Conway turn, then
  does its own faction passes around it. The core's rules are reused, not
  reimplemented.
- The new module (`src/golz.c`) is **also pure** — no LVGL, no Redis — so it earns
  the same frameworkless host-test treatment as the core (`tests/test_golz.c`).

Four properties of the base core make this safe and cheap. Check for them before
reaching for this pattern:

1. **Read-one-buffer / write-another step.** `gol_step` reads only `cur` and writes
   `next` (then swaps). Because it never reads its own mid-step output, the wrapping
   layer can **post-process `living.cur` after the step returns** — suppress births
   that landed on zombie cells, clear cells that were eaten — without corrupting any
   core invariant. This is the linchpin. A core that mutated cells in place mid-sweep
   would not be safe to wrap this way.

2. **A threadable RNG seam.** The core draws randomness through
   `gol_rand_u32(uint32_t *state)`. The new layer borrows the *same* `g->rng`
   pointer, so both factions advance one shared deterministic stream — preserving
   reproducibility and host-testability across the whole composite.

3. **Reusable read-only analyzers.** Cycle/extinction detection
   (`gol_cycle_record`) hashes `living.cur` only. The composite's terminal logic
   (`golz_terminal`) reuses it directly on the embedded layer — the moving zombies
   simply aren't part of the living hash, which is exactly the desired semantics
   ("the living population stabilized") for free.

4. **Reusable render primitives.** `gol_channel_intensity(alive, trail_age,
   trail_turns)` is reused unchanged for the zombie (red) trail fade; only the final
   pixel composition is a new pure helper (`golz_compose_pixel`).

The discipline the pattern demands in return:

- **Snapshot before you mutate.** Capture `prev_living` before the living turn (death
  diff baseline) and a `snapshot` of the post-movement living grid before the
  eat/kill pass, so all per-step decisions read a frozen state and order-independence
  holds.
- **Fix the phase order and write it down.** GoLZ's step is a strict pipeline:
  promote deferred zombies -> snapshot `prev_living` -> reset `died_mask` -> living
  turn (`gol_step` + birth suppression + death recording) -> movement -> eat/kill
  snapshot pass -> spawn -> trail update -> `generation++`.
- **Prove non-interference with a parity test.** The strongest regression guard is a
  host test asserting the wrapped layer is **bit-identical to bare `gol_step`** when
  the second faction is absent (`test_living_parity_no_zombies`).

## Why This Matters

- **Blast radius.** The shipping Game of Life mode and its 17+ host-test suites keep
  running on untouched code. A bug in GoLZ can never regress GoL because they share
  no mutable surface beyond the core's *read-only* reuse.
- **Smaller, reviewable change.** The feature is additive: one new pure module, one
  new mode, a handful of Redis helpers. The correctness review verified seven
  invariants without re-auditing `gol_step`.
- **Testability dividend.** Because the new layer is pure, the hard logic
  (eat/kill buckets, reinfection bookkeeping, spawn math, terminal precedence) is
  covered by deterministic host tests; only the LVGL/Redis glue needs
  hardware verification. Generalizing the core would have pushed new logic *into* the
  hot path, where it is harder to isolate.
- **Honest semantics.** Reusing the living-only cycle hash gave the exactly-right
  "zombies win is checked before stalemate restart" behavior with no new detector.

The tradeoff is memory: parallel grids cost extra `cols*rows` bytes per buffer
versus a single widened cell array. On this panel (worst case ~211k cells) that is a
few hundred KB — a non-issue against the clarity and safety won. If per-cell state
were large or the grids enormous, a generalized cell struct might win instead.

## When to Apply

- The new feature reads as "*the existing simulation* **plus** another interacting
  thing," and the base simulation should still exist on its own.
- The base core's step is read-one-buffer / write-another (safe to post-process its
  output), exposes a threadable RNG seam, and is already well-tested.
- You catch yourself about to widen the core's cell type or add a faction branch to
  its hottest loop **purely to serve a second consumer**.

**Do not** apply when the two layers genuinely need shared *mutable* per-cell state
every sub-step (true 3-state cellular rules where the factions are not separable), or
when the base core would need behavioral changes anyway — then generalizing (or
forking) the core is the more honest move than bolting parallel grids onto a core
that fights you.

## Examples

Composition over generalization — embed the core, add parallel layers:

```c
/* src/golz.h — the composite owns an ordinary GoL board plus faction grids */
typedef struct {
    gol_t     living;      /* reused verbatim: the living faction is a GoL board */
    uint8_t  *zombies;     /* active zombie grid (parallel to living.cur)        */
    uint8_t  *z_new;       /* born-this-gen; promoted at next step start (R12)   */
    uint8_t  *z_trail;     /* zombie fade trail (mirrors gol_t.trail)            */
    uint8_t  *prev_living; /* death-diff baseline, snapshotted each step         */
    uint8_t  *snapshot;    /* frozen post-movement living grid for eat/kill      */
    uint8_t  *died_mask;   /* this gen's deaths only (reset every step)          */
    int      *empties;     /* reusable scan/output scratch (no per-step alloc)   */
    uint32_t *rng;         /* borrowed: same deterministic stream as the core    */
    /* ... */
} golz_t;
```

The step reuses `gol_step`, then post-processes its output (safe because the core
read only `cur`):

```c
void golz_step(golz_t *g) {
    gz_promote(g);                            /* activate deferred zombies (R12) */
    memcpy(g->prev_living, g->living.cur, n); /* baseline BEFORE the living turn */
    memset(g->died_mask, 0, n);
    gol_step(&g->living);                     /* <-- core reused VERBATIM        */
    /* post-process the core's output: suppress births on zombie cells, record
       genuine Conway deaths; safe because gol_step never re-reads living.cur   */
    gz_living_turn_postprocess(g);
    gz_move(g);
    gz_eat_kill(g);                           /* reads g->snapshot, not live cur */
    gz_spawn(g);
    gz_update_ztrail(g);
    g->generation++;
}
```

Reusing the core's read-only analyzer for terminal logic — no new detector:

```c
golz_terminal_t golz_terminal(golz_t *g) {
    if (gol_live_count(&g->living) == 0 && golz_zombie_count(g) > 0)
        return GOLZ_ZOMBIE_WIN;                 /* checked BEFORE any restart    */
    if (g->generation >= (uint32_t)g->cfg.max_generations)
        return GOLZ_QUIET_RESTART;              /* unconditional backstop        */
    if (gol_cycle_record(&g->cycle, &g->living))/* hashes living.cur only —      */
        return GOLZ_QUIET_RESTART;              /* zombies aren't in the hash    */
    return GOLZ_CONTINUE;
}
```

The non-interference guarantee, locked in by a host test:

```c
/* tests/test_golz.c — with no zombies, the wrapped layer must be bit-identical
 * to bare gol_step, proving the new module never perturbs the reused core. */
static void test_living_parity_no_zombies(void) {
    /* seed two boards identically; step gol_t directly vs. via golz_step;
       assert living.cur matches every generation */
}
```

## Related

- [LVGL swipe-vs-tap gesture guard](lvgl-swipe-vs-tap-gesture-guard.md)
  — the companion pattern for the GoLZ tap menu / banner overlays reused from the GoL mode.
- Implementation plan: [docs/plans/2026-06-09-002-feat-golz-zombies-mode-plan.md](../../plans/2026-06-09-002-feat-golz-zombies-mode-plan.md)
  (Architecture Decisions section, "keep `gol_t`/`gol_step` unmodified").
- Prior art in the same codebase: the RGB tri-board Game of Life variant kept a single
  `gol_t` per board and composited in a pure `gol_compose_pixel` helper rather than
  widening the cell — the same "compose, don't widen" instinct at the render layer.
