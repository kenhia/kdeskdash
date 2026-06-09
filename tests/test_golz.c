/**
 * @file test_golz.c
 * Host-only unit tests for the pure GoLZ core (no LVGL/Redis dependency).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "golz.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void check_eq(int got, int want, const char *what) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %d, want %d\n", what, got, want);
        failures++;
    }
}

static golz_settings_t mk_cfg(int initial, int reinfect, int spawn) {
    golz_settings_t z = {.initial_count = initial,
                         .zombie_reinfect = reinfect,
                         .zombie_spawn_chance = spawn,
                         .max_generations = 1000};
    return z;
}

/* init allocates buffers; free is safe on a zeroed struct and after init. */
static void test_init_free(void) {
    golz_t zeroed;
    memset(&zeroed, 0, sizeof(zeroed));
    golz_free(&zeroed); /* must not crash on a zeroed struct */

    gol_settings_t living = {.density = 0.3, .trail = true, .trail_turns = 6};
    golz_settings_t zcfg = mk_cfg(3, 10, 20);
    uint32_t rng = 12345;
    golz_t g;
    check(golz_init(&g, 16, 8, &living, &zcfg, &rng), "golz_init succeeds");
    check(g.zombies && g.z_new && g.prev_living && g.snapshot && g.died_mask &&
              g.empties,
          "all grids allocated");
    check_eq(golz_zombie_count(&g), 0, "fresh board has no zombies");
    golz_free(&g);
}

/* settings_clamp keeps values inside valid bounds. */
static void test_settings_clamp(void) {
    golz_settings_t c = {.initial_count = 99,
                         .zombie_reinfect = -5,
                         .zombie_spawn_chance = 250,
                         .max_generations = 0};
    golz_settings_clamp(&c);
    check_eq(c.initial_count, 5, "initial_count clamps to 5");
    check_eq(c.zombie_reinfect, 0, "reinfect clamps to 0");
    check_eq(c.zombie_spawn_chance, 100, "spawn_chance clamps to 100");
    check_eq(c.max_generations, 1, "max_generations floors at 1");
}

/* seed places exactly initial_count zombies, all on empty cells, and is
 * reproducible for a fixed starting PRNG state. */
static void test_seed_places_zombies(void) {
    gol_settings_t living = {.density = 0.3, .trail_turns = 1};
    golz_settings_t zcfg = mk_cfg(5, 0, 0);

    uint32_t rng = 0xC0FFEE;
    golz_t g;
    golz_init(&g, 30, 10, &living, &zcfg, &rng);
    golz_seed(&g);
    check_eq(golz_zombie_count(&g), 5, "seed places exactly initial_count");

    size_t n = (size_t)g.cols * g.rows;
    bool all_on_empty = true;
    for (size_t i = 0; i < n; i++)
        if (g.zombies[i] && g.living.cur[i])
            all_on_empty = false;
    check(all_on_empty, "every zombie sits on a non-living cell");

    /* Capture positions, reseed from the same starting state, compare. */
    uint8_t first[300];
    memcpy(first, g.zombies, n);
    uint8_t living_first[300];
    memcpy(living_first, g.living.cur, n);

    rng = 0xC0FFEE;
    golz_seed(&g);
    check(memcmp(first, g.zombies, n) == 0, "zombie placement is deterministic");
    check(memcmp(living_first, g.living.cur, n) == 0,
          "living seed is deterministic");
    golz_free(&g);
}

/* initial_count 0 places no zombies and the living layer matches a plain GoL
 * seed drawn from the same starting PRNG state. */
static void test_seed_zero_zombies(void) {
    gol_settings_t living = {.density = 0.4, .trail_turns = 1};
    golz_settings_t zcfg = mk_cfg(0, 0, 0);

    uint32_t rng_a = 777;
    golz_t g;
    golz_init(&g, 24, 12, &living, &zcfg, &rng_a);
    golz_seed(&g);
    check_eq(golz_zombie_count(&g), 0, "initial_count 0 -> no zombies");

    uint32_t rng_b = 777;
    gol_t plain;
    gol_init(&plain, 24, 12, &living);
    gol_seed(&plain, &rng_b);

    size_t n = (size_t)g.cols * g.rows;
    check(memcmp(g.living.cur, plain.cur, n) == 0,
          "living layer matches a plain GoL seed when no zombies are placed");
    gol_free(&plain);
    golz_free(&g);
}

/* sampler returns only the available count, with distinct in-bounds empty
 * indices, when asked for more than exist. */
static void test_sampler_overrequest(void) {
    gol_settings_t living = {.trail_turns = 1};
    golz_settings_t zcfg = mk_cfg(0, 0, 0);
    uint32_t rng = 42;
    golz_t g;
    golz_init(&g, 4, 4, &living, &zcfg, &rng); /* 16 cells */

    /* Occupy 13 cells (12 living + 1 zombie), leaving 3 empties. */
    for (int i = 0; i < 12; i++)
        g.living.cur[i] = 1;
    g.zombies[12] = 1;

    int k = golz_sample_empty(&g, 100);
    check_eq(k, 3, "sampler caps at available empties");

    bool distinct = true, all_empty = true;
    for (int i = 0; i < k; i++) {
        int idx = g.empties[i];
        if (idx < 0 || idx >= 16 || g.living.cur[idx] || g.zombies[idx] ||
            g.z_new[idx])
            all_empty = false;
        for (int j = i + 1; j < k; j++)
            if (g.empties[i] == g.empties[j])
                distinct = false;
    }
    check(distinct, "sampled indices are distinct");
    check(all_empty, "sampled indices are all empty and in bounds");
    golz_free(&g);
}

/* sampler treats a z_new cell as occupied (never spawns onto a reinfected
 * cell) and returns 0 on a fully occupied board. */
static void test_sampler_excludes_znew_and_full(void) {
    gol_settings_t living = {.trail_turns = 1};
    golz_settings_t zcfg = mk_cfg(0, 0, 0);
    uint32_t rng = 9;
    golz_t g;
    golz_init(&g, 3, 3, &living, &zcfg, &rng); /* 9 cells */

    g.z_new[4] = 1; /* center reinfected this gen */
    int k = golz_sample_empty(&g, 9);
    check_eq(k, 8, "z_new cell excluded from empties");
    bool avoids_center = true;
    for (int i = 0; i < k; i++)
        if (g.empties[i] == 4)
            avoids_center = false;
    check(avoids_center, "sampler never returns the z_new cell");

    /* Fill the board completely -> no empties. */
    for (int i = 0; i < 9; i++)
        g.living.cur[i] = 1;
    g.z_new[4] = 0;
    check_eq(golz_sample_empty(&g, 5), 0, "fully occupied board yields 0");
    golz_free(&g);
}

/* Conway living turn matches gol_step exactly when no zombies are present. */
static void test_living_parity_no_zombies(void) {
    gol_settings_t living = {.trail = true, .trail_turns = 4};
    golz_settings_t zcfg = mk_cfg(0, 0, 0);
    uint32_t rng = 1;
    golz_t g;
    golz_init(&g, 8, 8, &living, &zcfg, &rng);
    gol_set(&g.living, 3, 2, true);
    gol_set(&g.living, 3, 3, true);
    gol_set(&g.living, 3, 4, true);

    gol_t plain;
    gol_init(&plain, 8, 8, &living);
    gol_set(&plain, 3, 2, true);
    gol_set(&plain, 3, 3, true);
    gol_set(&plain, 3, 4, true);

    bool match = true;
    for (int gen = 0; gen < 6 && match; gen++) {
        golz_step(&g);
        gol_step(&plain);
        if (memcmp(g.living.cur, plain.cur, 64) != 0)
            match = false;
    }
    check(match, "living turn matches gol_step with no zombies");
    check_eq(golz_zombie_count(&g), 0, "no zombies appear from nowhere");
    gol_free(&plain);
    golz_free(&g);
}

/* A Conway birth landing on a zombie cell is suppressed: no living cell, no
 * trail, and the zombie itself survives. */
static void test_birth_suppression(void) {
    gol_settings_t living = {.trail = true, .trail_turns = 4};
    golz_settings_t zcfg = mk_cfg(0, 0, 0);
    uint32_t rng = 5;
    golz_t g;
    golz_init(&g, 9, 9, &living, &zcfg, &rng);
    /* three living neighbours of (3,3) -> Conway would birth (3,3) */
    gol_set(&g.living, 2, 3, true);
    gol_set(&g.living, 4, 3, true);
    gol_set(&g.living, 3, 2, true);
    g.zombies[3 * 9 + 3] = 1; /* zombie sits on the birth cell */

    golz_step(&g);
    check(!gol_get(&g.living, 3, 3), "birth onto a zombie cell is suppressed");
    check_eq(gol_trail(&g.living, 3, 3), 0,
             "no phantom trail seeded on a suppressed birth");
    check_eq(golz_zombie_count(&g), 1,
             "suppression does not destroy the zombie");
    golz_free(&g);
}

/* Zombies are never Conway neighbours: a living cell with three living
 * neighbours survives even with an adjacent zombie (which, if miscounted as a
 * fourth neighbour, would kill it by overpopulation). */
static void test_zombies_not_neighbors(void) {
    gol_settings_t living = {.trail_turns = 1};
    golz_settings_t zcfg = mk_cfg(0, 0, 0);
    uint32_t rng = 7;
    golz_t g;
    golz_init(&g, 10, 10, &living, &zcfg, &rng);
    /* target (5,4) with exactly three living neighbours */
    gol_set(&g.living, 5, 4, true);
    gol_set(&g.living, 5, 5, true);
    gol_set(&g.living, 4, 4, true);
    gol_set(&g.living, 6, 4, true);
    g.zombies[3 * 10 + 5] = 1; /* zombie at (5,3), a fourth (non-)neighbour */

    golz_step(&g);
    check(gol_get(&g.living, 5, 4),
          "living cell survives: adjacent zombie is not a Conway neighbour");
    golz_free(&g);
}

/* A lone zombie on an otherwise empty board always moves to a toroidal
 * neighbour of its origin. */
static void test_lone_zombie_moves(void) {
    gol_settings_t living = {.trail_turns = 1};
    golz_settings_t zcfg = mk_cfg(0, 0, 0);
    uint32_t rng = 0xBEEF;
    golz_t g;
    golz_init(&g, 10, 10, &living, &zcfg, &rng);
    int ox = 5, oy = 5;
    g.zombies[oy * 10 + ox] = 1;

    golz_step(&g);
    check_eq(golz_zombie_count(&g), 1, "lone zombie count conserved");
    check(g.zombies[oy * 10 + ox] == 0,
          "lone zombie leaves its origin (all neighbours empty)");
    int found = -1;
    for (int i = 0; i < 100; i++)
        if (g.zombies[i])
            found = i;
    int fx = found % 10, fy = found / 10;
    int ddx = abs(fx - ox);
    if (ddx > 5)
        ddx = 10 - ddx;
    int ddy = abs(fy - oy);
    if (ddy > 5)
        ddy = 10 - ddy;
    check(found >= 0 && ddx <= 1 && ddy <= 1 && !(ddx == 0 && ddy == 0),
          "moved to an adjacent (toroidal) cell");
    golz_free(&g);
}

/* A lone zombie at corner (0,0) moves to a toroidal neighbour, exercising the
 * wrap to the opposite edges. */
static void test_toroidal_wrap_move(void) {
    gol_settings_t living = {.trail_turns = 1};
    golz_settings_t zcfg = mk_cfg(0, 0, 0);
    uint32_t rng = 0x1234;
    golz_t g;
    golz_init(&g, 8, 8, &living, &zcfg, &rng);
    g.zombies[0] = 1; /* (0,0) */

    golz_step(&g);
    int found = -1;
    for (int i = 0; i < 64; i++)
        if (g.zombies[i])
            found = i;
    int fx = found % 8, fy = found / 8;
    int ddx = fx;
    if (ddx > 4)
        ddx = 8 - ddx;
    int ddy = fy;
    if (ddy > 4)
        ddy = 8 - ddy;
    check(found >= 0 && ddx <= 1 && ddy <= 1 && !(ddx == 0 && ddy == 0),
          "corner zombie wraps toroidally to an adjacent cell");
    golz_free(&g);
}

/* When every cell is a zombie, every neighbour is occupied so no zombie moves
 * (single attempt, blocked -> stay). */
static void test_fully_blocked_no_move(void) {
    gol_settings_t living = {.trail_turns = 1};
    golz_settings_t zcfg = mk_cfg(0, 0, 0);
    uint32_t rng = 3;
    golz_t g;
    golz_init(&g, 5, 5, &living, &zcfg, &rng);
    for (int i = 0; i < 25; i++)
        g.zombies[i] = 1;
    uint8_t before[25];
    memcpy(before, g.zombies, 25);

    golz_step(&g);
    check_eq(golz_zombie_count(&g), 25, "fully-zombie board conserves count");
    check(memcmp(before, g.zombies, 25) == 0,
          "no zombie moves when every neighbour is occupied");
    golz_free(&g);
}

/* died_mask records exactly this generation's Conway deaths and is reset each
 * step (no accumulation across generations). */
static void test_died_mask_and_reset(void) {
    gol_settings_t living = {.trail_turns = 1};
    golz_settings_t zcfg = mk_cfg(0, 0, 0);
    uint32_t rng = 11;
    golz_t g;
    golz_init(&g, 7, 7, &living, &zcfg, &rng);
    gol_set(&g.living, 3, 2, true);
    gol_set(&g.living, 3, 3, true);
    gol_set(&g.living, 3, 4, true);

    golz_step(&g);
    check(g.died_mask[2 * 7 + 3] == 1, "blinker end (3,2) recorded as died");
    check(g.died_mask[4 * 7 + 3] == 1, "blinker end (3,4) recorded as died");
    check(g.died_mask[3 * 7 + 3] == 0, "blinker centre not recorded as died");
    int dc = 0;
    for (int i = 0; i < 49; i++)
        dc += g.died_mask[i];
    check_eq(dc, 2, "exactly two Conway deaths recorded");

    golz_step(&g);
    check(g.died_mask[2 * 7 + 3] == 0, "stale death (3,2) cleared next gen");
    check(g.died_mask[4 * 7 + 3] == 0, "stale death (3,4) cleared next gen");
    check(g.died_mask[3 * 7 + 2] == 1 && g.died_mask[3 * 7 + 4] == 1,
          "this generation's deaths recorded");
    dc = 0;
    for (int i = 0; i < 49; i++)
        dc += g.died_mask[i];
    check_eq(dc, 2, "died_mask reflects only the latest generation");
    golz_free(&g);
}

/* A z_new zombie is inactive until promoted at the start of the next step. */
static void test_znew_promotion(void) {
    gol_settings_t living = {.trail_turns = 1};
    golz_settings_t zcfg = mk_cfg(0, 0, 0);
    uint32_t rng = 99;
    golz_t g;
    golz_init(&g, 8, 8, &living, &zcfg, &rng);
    g.z_new[3 * 8 + 3] = 1;
    check_eq(golz_zombie_count(&g), 0, "z_new is not an active zombie yet");

    golz_step(&g);
    check_eq(golz_zombie_count(&g), 1, "z_new promoted to an active zombie");
    int zc = 0;
    for (int i = 0; i < 64; i++)
        zc += g.z_new[i];
    check_eq(zc, 0, "z_new grid cleared after promotion");
    golz_free(&g);
}

/* golz_step is deterministic: two boards seeded and stepped from the same PRNG
 * state evolve identically. */
static void test_step_deterministic(void) {
    gol_settings_t living = {.density = 0.3, .trail_turns = 2};
    golz_settings_t zcfg = mk_cfg(4, 10, 20);
    uint32_t r1 = 24680, r2 = 24680;
    golz_t a, b;
    golz_init(&a, 20, 10, &living, &zcfg, &r1);
    golz_init(&b, 20, 10, &living, &zcfg, &r2);
    golz_seed(&a);
    golz_seed(&b);
    check(memcmp(a.zombies, b.zombies, 200) == 0, "identical seeds match");
    for (int s = 0; s < 5; s++) {
        golz_step(&a);
        golz_step(&b);
    }
    check(memcmp(a.living.cur, b.living.cur, 200) == 0 &&
              memcmp(a.zombies, b.zombies, 200) == 0,
          "identical PRNG state yields identical evolution");
    golz_free(&a);
    golz_free(&b);
}

int main(void) {
    test_init_free();
    test_settings_clamp();
    test_seed_places_zombies();
    test_seed_zero_zombies();
    test_sampler_overrequest();
    test_sampler_excludes_znew_and_full();
    test_living_parity_no_zombies();
    test_birth_suppression();
    test_zombies_not_neighbors();
    test_lone_zombie_moves();
    test_toroidal_wrap_move();
    test_fully_blocked_no_move();
    test_died_mask_and_reset();
    test_znew_promotion();
    test_step_deterministic();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_golz: all passed\n");
    return 0;
}
