/**
 * @file test_golz.c
 * Host-only unit tests for the pure GoLZ core (no LVGL/Redis dependency).
 */
#include <stdio.h>
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

int main(void) {
    test_init_free();
    test_settings_clamp();
    test_seed_places_zombies();
    test_seed_zero_zombies();
    test_sampler_overrequest();
    test_sampler_excludes_znew_and_full();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_golz: all passed\n");
    return 0;
}
