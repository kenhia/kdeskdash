/**
 * @file gol.h
 * Pure Conway's Game of Life core: toroidal grid, density seeding, and an
 * optional fade trail. No LVGL/DRM dependency, so it is host-testable.
 *
 * Cells are stored row-major. The grid is toroidal: edges wrap, so neighbours
 * of an edge cell include cells on the opposite edge(s).
 */
#ifndef KDESKDASH_GOL_H
#define KDESKDASH_GOL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int    cell_size;   /* px per cell side (e.g. 3 -> 3x3) */
    int    padding;     /* px gap between cells (0 = touching) */
    double density;     /* fraction of cells alive at generation 0 (0..1) */
    bool   trail;       /* dead cells fade out over trail_turns instead of off */
    int    trail_turns; /* generations a trail takes to fade to dark (>= 1) */
    int    speed_ms;    /* delay between generations */
} gol_settings_t;

typedef struct {
    int      cols;
    int      rows;
    uint8_t *cur;   /* alive flags, 0/1, row-major (cols*rows) */
    uint8_t *next;  /* scratch buffer for the next generation */
    uint8_t *trail; /* fade intensity 0..trail_turns, row-major */
    gol_settings_t cfg;
} gol_t;

/* Allocate grid buffers for a cols x rows board. Returns false on bad args or
 * allocation failure. The board starts fully dead. */
bool gol_init(gol_t *g, int cols, int rows, const gol_settings_t *cfg);

/* Release grid buffers (safe to call on a zeroed/partly-initialised gol_t). */
void gol_free(gol_t *g);

/* Set every cell dead and clear all trails. */
void gol_clear(gol_t *g);

/* Get/set a single cell's alive state (no-op/false when out of bounds). */
void gol_set(gol_t *g, int x, int y, bool alive);
bool gol_get(const gol_t *g, int x, int y);

/* Trail intensity for a cell (0 = dark, cfg.trail_turns = full). */
uint8_t gol_trail(const gol_t *g, int x, int y);

/* Number of currently-alive cells. */
int gol_live_count(const gol_t *g);

/* Populate generation 0 randomly to cfg.density using the caller's PRNG state
 * (advanced in place, so seeding is reproducible in tests). */
void gol_seed(gol_t *g, uint32_t *rng_state);

/* Advance one generation (toroidal Conway rules) and update trails. */
void gol_step(gol_t *g);

/* Small deterministic PRNG (xorshift32) used for seeding; exposed so callers
 * and tests share the same stream. Never returns 0 state. */
uint32_t gol_rand_u32(uint32_t *state);

#endif /* KDESKDASH_GOL_H */
