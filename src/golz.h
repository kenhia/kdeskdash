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
    int machete_percentage;  /* % chance each cell is seeded with a machete (0..100) */
    int human_kill_zombie;   /* % chance a machete-armed human kills an in-range
                              * zombie when one is present (0..100) */
    int generations_to_win;  /* humans win by lasting this many generations;
                              * <= 0 disables the human-win threshold */
} golz_settings_t;

typedef struct {
    gol_t living;         /* reused verbatim; living.cur is the living layer */
    uint8_t *machetes;    /* static machete layer, 0/1; seeded once, never changes */
    uint8_t *zombies;     /* active zombies, 0/1, row-major (cols*rows) */
    uint8_t *z_new;       /* zombies born this gen (reinfect/spawn); promoted next step */
    uint8_t *z_trail;     /* zombie (red) fade trail 0..trail_turns; mirrors gol_t.trail */
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
 * max_generations >= 1, machete_percentage/human_kill_zombie [0,100],
 * generations_to_win >= 0 (0 disables the human-win threshold). Blank/unset
 * fields are left as-is by the caller. */
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

/* Seed a new round: density-seed the living layer, place cfg.initial_count
 * zombies on empty cells, then roll the static machete layer (each cell
 * independently at cfg.machete_percentage). The machete roll is drawn LAST and is
 * skipped entirely when machete_percentage == 0, so existing PRNG streams are
 * unperturbed. Reproducible for a given starting PRNG state. */
void golz_seed(golz_t *g);

/* Collect every empty cell (living==0 && zombies==0 && z_new==0) into the
 * pre-allocated scratch, partial-Fisher-Yates the first min(want, available)
 * entries, and return that count. The chosen cell indices are left in
 * g->empties[0..return-1]. Draws from g->rng. No allocation in the hot path. */
int golz_sample_empty(golz_t *g, int want);

/* Number of currently-active zombies (excludes z_new). */
int golz_zombie_count(const golz_t *g);

/* The action a zombie takes given its count of (frozen-snapshot) living
 * neighbours: eat one for 1-2, get killed for 4-5, otherwise nothing. */
typedef enum {
    GOLZ_EAT_NONE = 0,
    GOLZ_EAT_ONE,
    GOLZ_EAT_KILLED,
} golz_eat_action_t;

golz_eat_action_t golz_eat_action(int living_neighbors);

/* Number of zombies to spawn from `deaths` deaths at a drawn percentage
 * `pct` (1..30): ceil(pct/100 * deaths), floored at 1 when deaths > 0, else 0. */
int golz_spawn_count(int pct, int deaths);

/* Compose one XRGB8888 pixel for cell (x,y): living -> green (or BLUE when the
 * cell also holds a machete), zombie -> red (live occupant overrides any trail);
 * an empty cell fades its own-colour trails (green/blue for living, red for
 * zombie) via gol_channel_intensity. Because machetes are static, a fading living
 * trail is blue iff that cell holds a machete. */
uint32_t golz_compose_pixel(const golz_t *g, int x, int y);

/* Advance one full GoLZ generation: promote z_new, run the Conway living turn
 * (birth suppression + death recording), the MACHETE turn (armed humans kill an
 * in-range zombie), the zombie movement pass, the snapshot-based eat/kill +
 * reinfect pass, the spawn-from-the-dead pass, and the zombie trail update.
 * Advances the generation counter. */
void golz_step(golz_t *g);

/* Per-generation terminal decision, evaluated by the mode after golz_step.
 * Precedence: (1) zombie win = no living and >=1 zombie; (2) human win =
 * cfg.generations_to_win > 0 and living remain and the generation counter reached
 * it; (3) tie = living-cells-only cycle/extinction via the reused hash ring, or
 * the cfg.max_generations backstop. Otherwise the round continues. Advances the
 * cycle ring, so call exactly once per generation. */
typedef enum {
    GOLZ_CONTINUE = 0,
    GOLZ_ZOMBIE_WIN,
    GOLZ_HUMAN_WIN,
    GOLZ_TIE,
} golz_terminal_t;

golz_terminal_t golz_terminal(golz_t *g);

/* Parse a persisted win-counter string into a non-negative count, returning
 * `fallback` for NULL/empty/non-numeric/trailing-junk/negative input. Pure;
 * used by the Redis win-counter read. */
long golz_parse_wins(const char *s, long fallback);

#endif /* KDESKDASH_GOLZ_H */
