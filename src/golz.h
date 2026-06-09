/**
 * @file golz.h
 * Pure "Game of Life with Zombies" (GoLZ) core: a two-faction simulation built
 * on top of the unmodified Conway core in gol.h. The living population is an
 * ordinary toroidal `gol_t`; a parallel zombie faction moves, eats, reinfects,
 * spawns from the dead, and can win by wiping out all life.
 *
 * No LVGL/DRM/Redis dependency, so the whole ruleset is host-testable like the
 * Conway core. All randomness is drawn from a caller-owned PRNG state via
 * gol_rand_u32, so runs are reproducible in tests.
 */
#ifndef KDESKDASH_GOLZ_H
#define KDESKDASH_GOLZ_H

#include <stdbool.h>
#include <stdint.h>

#include "gol.h"

typedef struct {
    int initial_count;       /* zombies placed at seed (0..5) */
    int zombie_reinfect;     /* % chance an eaten cell becomes a zombie (0..100) */
    int zombie_spawn_chance; /* % chance to spawn from the dead each gen (0..100) */
    int max_generations;     /* unconditional restart backstop (>= 1) */
} golz_settings_t;

typedef struct {
    gol_t living;         /* reused verbatim; living.cur is the living layer */
    uint8_t *zombies;     /* active zombies, 0/1, row-major (cols*rows) */
    uint8_t *z_new;       /* zombies born this gen (reinfect/spawn); promoted next step */
    uint8_t *prev_living; /* copy of living.cur taken BEFORE gol_step (death diff) */
    uint8_t *snapshot;    /* copy of living.cur taken AFTER movement (eat/kill reads) */
    uint8_t *died_mask;   /* living cells that died this gen; reset each step */
    int     *empties;     /* sampler scratch (cols*rows ints); allocated once at init */
    int      cols;
    int      rows;
    golz_settings_t cfg;
    uint32_t *rng;        /* caller-owned PRNG seam (gol_rand_u32) */
    gol_cycle_t cycle;    /* living-cells-only hash ring (reused) */
    uint32_t generation;  /* generations advanced since the last seed */
} golz_t;

/* Clamp settings into valid bounds: initial_count [0,5], reinfect/spawn [0,100],
 * max_generations >= 1. Blank/unset fields are left as-is by the caller. */
void golz_settings_clamp(golz_settings_t *cfg);

/* Allocate the living layer plus all parallel zombie grids and the sampler
 * scratch (each cols*rows), once. `rng` is borrowed (not owned). Returns false
 * on bad args or allocation failure. Settings are clamped. */
bool golz_init(golz_t *g, int cols, int rows, const gol_settings_t *living_cfg,
               const golz_settings_t *cfg, uint32_t *rng);

/* Release all buffers (safe on a zeroed/partly-initialised golz_t). */
void golz_free(golz_t *g);

/* Reset every layer to empty and clear the cycle ring + generation counter. */
void golz_clear(golz_t *g);

/* Seed a new round: density-seed the living layer, then place cfg.initial_count
 * zombies on empty cells. Reproducible for a given starting PRNG state. */
void golz_seed(golz_t *g);

/* Collect every empty cell (living==0 && zombies==0 && z_new==0) into the
 * pre-allocated scratch, partial-Fisher-Yates the first min(want, available)
 * entries, and return that count. The chosen cell indices are left in
 * g->empties[0..return-1]. Draws from g->rng. No allocation in the hot path. */
int golz_sample_empty(golz_t *g, int want);

/* Number of currently-active zombies (excludes z_new). */
int golz_zombie_count(const golz_t *g);

#endif /* KDESKDASH_GOLZ_H */
