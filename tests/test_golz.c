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

/* The eat/kill action table (R8). */
static void test_eat_action_table(void) {
    check(golz_eat_action(0) == GOLZ_EAT_NONE, "0 neighbours -> nothing");
    check(golz_eat_action(1) == GOLZ_EAT_ONE, "1 neighbour -> eat one");
    check(golz_eat_action(2) == GOLZ_EAT_ONE, "2 neighbours -> eat one");
    check(golz_eat_action(3) == GOLZ_EAT_NONE, "3 neighbours -> standoff");
    check(golz_eat_action(4) == GOLZ_EAT_KILLED, "4 neighbours -> killed");
    check(golz_eat_action(5) == GOLZ_EAT_KILLED, "5 neighbours -> killed");
    check(golz_eat_action(6) == GOLZ_EAT_NONE, "6 neighbours -> survives");
    check(golz_eat_action(7) == GOLZ_EAT_NONE, "7 neighbours -> survives");
    check(golz_eat_action(8) == GOLZ_EAT_NONE, "8 neighbours -> survives");
}

/* Spawn-count math: ceil(pct/100 * deaths), floored at 1 when deaths > 0 (R10). */
static void test_spawn_count_math(void) {
    check_eq(golz_spawn_count(30, 0), 0, "no deaths -> 0 spawn");
    check_eq(golz_spawn_count(1, 1), 1, "deaths 1, 1pct -> floor at 1");
    check_eq(golz_spawn_count(30, 1), 1, "deaths 1, 30pct -> 1");
    check_eq(golz_spawn_count(1, 10), 1, "deaths 10, 1pct -> ceil(0.1)=1");
    check_eq(golz_spawn_count(15, 10), 2, "deaths 10, 15pct -> ceil(1.5)=2");
    check_eq(golz_spawn_count(30, 10), 3, "deaths 10, 30pct -> 3");
    check_eq(golz_spawn_count(10, 100), 10, "deaths 100, 10pct -> 10");
    bool ok = true;
    for (int p = 1; p <= 30; p++) {
        int v = golz_spawn_count(p, 10);
        if (v < 1 || v > 3)
            ok = false;
    }
    check(ok, "deaths 10 across 1..30pct stays in 1..3");
}

/* Spawn presence/absence: a forced roll with deaths>0 spawns; spawn_chance 0
 * never spawns; zero deaths spawns nothing even at 100pct (R10/R11). */
static void test_spawn_presence(void) {
    gol_settings_t living = {.trail_turns = 1};

    /* one lone living cell -> exactly one Conway death, no zombies */
    golz_settings_t hit = mk_cfg(0, 0, 100);
    uint32_t r1 = 1;
    golz_t g;
    golz_init(&g, 20, 5, &living, &hit, &r1);
    gol_set(&g.living, 10, 2, true);
    golz_step(&g);
    int zn = 0;
    for (int i = 0; i < 100; i++)
        zn += g.z_new[i];
    check_eq(zn, 1, "deaths=1 with spawn_chance 100 spawns exactly one");
    golz_free(&g);

    golz_settings_t miss = mk_cfg(0, 0, 0);
    uint32_t r2 = 1;
    golz_t g2;
    golz_init(&g2, 20, 5, &living, &miss, &r2);
    gol_set(&g2.living, 10, 2, true);
    golz_step(&g2);
    zn = 0;
    for (int i = 0; i < 100; i++)
        zn += g2.z_new[i];
    check_eq(zn, 0, "spawn_chance 0 never spawns");
    golz_free(&g2);

    /* a 2x2 block is a still life -> zero deaths -> no spawn even at 100pct */
    golz_settings_t hit2 = mk_cfg(0, 0, 100);
    uint32_t r3 = 1;
    golz_t g3;
    golz_init(&g3, 8, 8, &living, &hit2, &r3);
    gol_set(&g3.living, 2, 2, true);
    gol_set(&g3.living, 3, 2, true);
    gol_set(&g3.living, 2, 3, true);
    gol_set(&g3.living, 3, 3, true);
    golz_step(&g3);
    zn = 0;
    for (int i = 0; i < 64; i++)
        zn += g3.z_new[i];
    check_eq(zn, 0, "zero deaths spawns nothing even at 100pct");
    golz_free(&g3);
}

/* With deaths=10 and a forced roll, the spawn count lands in 1..3 (R10). */
static void test_spawn_count_range(void) {
    gol_settings_t living = {.trail_turns = 1};
    golz_settings_t hit = mk_cfg(0, 0, 100);
    uint32_t rng = 12321;
    golz_t g;
    golz_init(&g, 30, 5, &living, &hit, &rng);
    for (int k = 0; k < 10; k++) /* 10 lone cells, spaced 3 apart */
        gol_set(&g.living, k * 3, 2, true);
    golz_step(&g);
    int zn = 0;
    for (int i = 0; i < 150; i++)
        zn += g.z_new[i];
    check(zn >= 1 && zn <= 3, "deaths=10 spawns 1..3");
    golz_free(&g);
}

/* Reinfect bookkeeping: with identical seeds and PRNG, a reinfect=100 run and a
 * reinfect=0 run differ only in where eaten cells are recorded -- B's deaths are
 * exactly A's deaths plus A's reinfected (disjoint), with identical living
 * outcomes (R9/R10). */
static void test_reinfect_bookkeeping(void) {
    gol_settings_t living = {.density = 0.45, .trail_turns = 3};
    golz_settings_t za = mk_cfg(0, 100, 0); /* reinfect 100, spawn 0 */
    golz_settings_t zb = mk_cfg(0, 0, 0);   /* reinfect 0,   spawn 0 */
    uint32_t ra = 555, rb = 555;
    golz_t a, b;
    golz_init(&a, 24, 10, &living, &za, &ra);
    golz_init(&b, 24, 10, &living, &zb, &rb);
    golz_seed(&a);
    golz_seed(&b);
    size_t n = 240;
    check(memcmp(a.living.cur, b.living.cur, n) == 0, "identical living seed");

    int placed = 0;
    for (int i = 0; i < (int)n && placed < 40; i++) {
        if (!a.living.cur[i]) {
            a.zombies[i] = 1;
            b.zombies[i] = 1;
            placed++;
        }
    }

    golz_step(&a);
    golz_step(&b);

    bool ok = true;
    int eaten = 0;
    for (size_t i = 0; i < n; i++) {
        if (a.died_mask[i] && a.z_new[i])
            ok = false; /* reinfected and death are disjoint */
        if (b.died_mask[i] != (a.died_mask[i] || a.z_new[i]))
            ok = false;
        if (a.living.cur[i] != b.living.cur[i])
            ok = false; /* same eats -> same living outcome */
        if (a.z_new[i])
            eaten++;
    }
    check(ok, "reinfect bookkeeping: B deaths == A deaths + A reinfected");
    check(eaten > 0, "scenario actually exercised eating/reinfection");
    golz_free(&a);
    golz_free(&b);
}

/* Render compose: living green, zombie red, occupant overrides trail, empty
 * cells fade own-colour trails (R19). */
static void test_compose_pixel(void) {
    gol_settings_t living = {.trail = true, .trail_turns = 8};
    golz_settings_t zcfg = mk_cfg(0, 0, 0);
    uint32_t rng = 1;
    golz_t g;
    golz_init(&g, 4, 4, &living, &zcfg, &rng);

    g.living.cur[0] = 1; /* (0,0) living */
    check(golz_compose_pixel(&g, 0, 0) == 0xFF00FF00u, "living -> full green");

    g.zombies[1] = 1; /* (1,0) zombie */
    check(golz_compose_pixel(&g, 1, 0) == 0xFFFF0000u, "zombie -> full red");

    g.zombies[2] = 1;
    g.living.trail[2] = 8; /* zombie over a green trail */
    check(golz_compose_pixel(&g, 2, 0) == 0xFFFF0000u,
          "zombie overrides existing green trail");

    g.living.cur[3] = 1;
    g.z_trail[3] = 8; /* living over a red trail */
    check(golz_compose_pixel(&g, 3, 0) == 0xFF00FF00u,
          "living overrides existing red trail");

    g.living.trail[4] = 4; /* (0,1) empty, half green trail -> 127 */
    check(golz_compose_pixel(&g, 0, 1) == (0xFF000000u | (127u << 8)),
          "empty green trail fades");

    g.z_trail[5] = 4; /* (1,1) empty, half red trail -> 127 */
    check(golz_compose_pixel(&g, 1, 1) == (0xFF000000u | (127u << 16)),
          "empty red trail fades");

    g.living.trail[6] = 8; /* (2,1) empty, full green + half red */
    g.z_trail[6] = 4;
    check(golz_compose_pixel(&g, 2, 1) ==
              (0xFF000000u | (127u << 16) | (255u << 8)),
          "empty cell composes both trails");

    check(golz_compose_pixel(&g, 3, 3) == 0xFF000000u, "fully empty -> black");
    golz_free(&g);
}

/* Zombie win: no living and at least one zombie -> ZOMBIE_WIN (R13). */
static void test_terminal_zombie_win(void) {
    gol_settings_t living = {.trail_turns = 1};
    golz_settings_t cfg = mk_cfg(0, 0, 0);
    uint32_t rng = 1;
    golz_t g;
    golz_init(&g, 16, 16, &living, &cfg, &rng);
    g.zombies[5 * 16 + 5] = 1; /* no living anywhere */
    check(golz_terminal(&g) == GOLZ_ZOMBIE_WIN, "no living + zombie -> win");
    golz_free(&g);
}

/* Win precedence: even with the generation counter past the backstop, a win is
 * still reported first (R13 over R23). */
static void test_terminal_win_precedence(void) {
    gol_settings_t living = {.trail_turns = 1};
    golz_settings_t cfg = mk_cfg(0, 0, 0);
    cfg.max_generations = 3;
    uint32_t rng = 1;
    golz_t g;
    golz_init(&g, 16, 16, &living, &cfg, &rng);
    g.zombies[2 * 16 + 2] = 1;
    g.generation = 100; /* well past the cap */
    check(golz_terminal(&g) == GOLZ_ZOMBIE_WIN,
          "win precedes backstop when both apply");
    golz_free(&g);
}

/* Backstop: living still present and active, but the generation cap is hit ->
 * QUIET_RESTART, not a win (R23). */
static void test_terminal_backstop(void) {
    gol_settings_t living = {.trail_turns = 1};
    golz_settings_t cfg = mk_cfg(0, 0, 0);
    cfg.max_generations = 5;
    uint32_t rng = 7;
    golz_t g;
    golz_init(&g, 16, 16, &living, &cfg, &rng);
    gol_set(&g.living, 8, 8, true);
    gol_set(&g.living, 9, 8, true);
    g.generation = 5; /* exactly at the cap */
    check(golz_terminal(&g) == GOLZ_QUIET_RESTART,
          "generation at cap -> backstop restart");
    golz_free(&g);
}

/* R15: living present, no zombies, no cycle yet -> CONTINUE. */
static void test_terminal_continue(void) {
    gol_settings_t living = {.trail_turns = 1};
    golz_settings_t cfg = mk_cfg(0, 0, 0);
    uint32_t rng = 3;
    golz_t g;
    golz_init(&g, 16, 16, &living, &cfg, &rng);
    gol_set(&g.living, 5, 5, true); /* blinker (changes each gen) */
    gol_set(&g.living, 6, 5, true);
    gol_set(&g.living, 7, 5, true);
    check(golz_terminal(&g) == GOLZ_CONTINUE,
          "living, no zombies, no cycle -> continue");
    check(golz_zombie_count(&g) == 0, "no zombies in this scenario");
    golz_free(&g);
}

/* A still-life living region with a far-off wandering zombie still trips the
 * living-only cycle detector (the moving zombie is not hashed) -> QUIET_RESTART
 * before the zombie can reach the block (R16). */
static void test_terminal_cycle_ignores_zombie(void) {
    gol_settings_t living = {.trail_turns = 1};
    golz_settings_t cfg = mk_cfg(0, 0, 0);
    uint32_t rng = 99;
    golz_t g;
    golz_init(&g, 16, 16, &living, &cfg, &rng);
    gol_set(&g.living, 1, 1, true); /* 2x2 still-life block */
    gol_set(&g.living, 2, 1, true);
    gol_set(&g.living, 1, 2, true);
    gol_set(&g.living, 2, 2, true);
    g.zombies[15 * 16 + 15] = 1; /* far corner */

    golz_terminal_t t = GOLZ_CONTINUE;
    int gens = 0;
    for (; gens < 8; gens++) {
        golz_step(&g);
        t = golz_terminal(&g);
        if (t != GOLZ_CONTINUE)
            break;
    }
    check(t == GOLZ_QUIET_RESTART, "still-life living trips cycle restart");
    check(gol_live_count(&g.living) == 4, "block survived (zombie never reached)");
    golz_free(&g);
}

/* Both factions extinct (empty board) -> QUIET_RESTART (never a win). */
static void test_terminal_both_extinct(void) {
    gol_settings_t living = {.trail_turns = 1};
    golz_settings_t cfg = mk_cfg(0, 0, 0);
    uint32_t rng = 1;
    golz_t g;
    golz_init(&g, 16, 16, &living, &cfg, &rng);
    /* empty living, empty zombies: repeated empty hash trips the detector */
    golz_terminal_t t = GOLZ_CONTINUE;
    int gens = 0;
    for (; gens < 4; gens++) {
        t = golz_terminal(&g);
        if (t != GOLZ_CONTINUE)
            break;
    }
    check(t == GOLZ_QUIET_RESTART, "double extinction -> quiet restart");
    check(t != GOLZ_ZOMBIE_WIN, "double extinction is not a win");
    golz_free(&g);
}

/* Pure win-counter parse-and-clamp (R17 read path). */
static void test_parse_wins(void) {
    check_eq((int)golz_parse_wins("0", 5), 0, "\"0\" -> 0");
    check_eq((int)golz_parse_wins("42", 0), 42, "\"42\" -> 42");
    check_eq((int)golz_parse_wins(NULL, 7), 7, "NULL -> fallback");
    check_eq((int)golz_parse_wins("", 7), 7, "empty -> fallback");
    check_eq((int)golz_parse_wins("-3", 7), 7, "negative -> fallback");
    check_eq((int)golz_parse_wins("abc", 7), 7, "non-numeric -> fallback");
    check_eq((int)golz_parse_wins("12x", 7), 7, "trailing junk -> fallback");
    check_eq((int)golz_parse_wins("  9", 0), 9, "leading space tolerated -> 9");
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
    test_eat_action_table();
    test_spawn_count_math();
    test_spawn_presence();
    test_spawn_count_range();
    test_reinfect_bookkeeping();
    test_compose_pixel();
    test_terminal_zombie_win();
    test_terminal_win_precedence();
    test_terminal_backstop();
    test_terminal_continue();
    test_terminal_cycle_ignores_zombie();
    test_terminal_both_extinct();
    test_parse_wins();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_golz: all passed\n");
    return 0;
}
