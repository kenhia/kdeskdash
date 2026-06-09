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
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int    cell_size;   /* px per cell side (e.g. 3 -> 3x3) */
    int    padding;     /* px gap between cells (0 = touching) */
    double density;     /* fraction of cells alive at generation 0 (0..1) */
    bool   trail;       /* dead cells fade out over trail_turns instead of off */
    int    trail_turns; /* generations a trail takes to fade to dark (>= 1) */
    int    speed_ms;    /* delay between generations */
    bool   rgb;         /* run three independent boards composited as R/G/B */
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

/* Color intensity (0..255) for one board's cell: full when alive, a faded value
 * when only a trail remains, 0 when dark. trail_turns is clamped to >= 1. */
uint8_t gol_channel_intensity(bool alive, uint8_t trail_t, int trail_turns);

/* Compose one XRGB8888 pixel from up to three boards' channel intensities.
 * board_count == 1 -> single green board (byte-identical to the legacy render);
 * board_count == 3 -> c0/c1/c2 map to the red/green/blue channels. */
uint32_t gol_compose_pixel(uint8_t c0, uint8_t c1, uint8_t c2, int board_count);

/* --- Cycle detection -------------------------------------------------------
 * A fixed ring of recent generation hashes, used to spot a board that has
 * settled into a still life or a short-period oscillator. Pure (no LVGL). */
#define GOL_CYCLE_SLOTS 16

typedef struct {
    uint64_t slots[GOL_CYCLE_SLOTS]; /* recent hashes; index = counter % SLOTS */
    uint32_t counter;                /* total hashes recorded since reset */
} gol_cycle_t;

/* FNV-1a 64-bit hash over a byte buffer. */
uint64_t gol_fnv1a64(const void *data, size_t len);

/* Clear the ring (zero every slot and the counter). */
void gol_cycle_reset(gol_cycle_t *c);

/* Hash the board's current cells, test that hash against the up-to-16 stored
 * slots, then store it. Returns true when the hash repeats a stored value,
 * i.e. the board has returned to a state seen within the last 16 generations. */
bool gol_cycle_record(gol_cycle_t *c, const gol_t *g);

#endif /* KDESKDASH_GOL_H */
