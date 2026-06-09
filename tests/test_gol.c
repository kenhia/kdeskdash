/**
 * @file test_gol.c
 * Host-only unit tests for the pure Game of Life core (no LVGL dependency).
 */
#include <stdio.h>

#include "gol.h"

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

/* A vertical 3-cell blinker oscillates with period 2. */
static void test_blinker(void) {
    gol_settings_t cfg = {.trail_turns = 1};
    gol_t g;
    gol_init(&g, 5, 5, &cfg);
    gol_set(&g, 2, 1, true);
    gol_set(&g, 2, 2, true);
    gol_set(&g, 2, 3, true);

    gol_step(&g);
    /* now horizontal */
    check(gol_get(&g, 1, 2) && gol_get(&g, 2, 2) && gol_get(&g, 3, 2),
          "blinker gen1 horizontal");
    check(!gol_get(&g, 2, 1) && !gol_get(&g, 2, 3), "blinker gen1 verticals dead");

    gol_step(&g);
    /* back to vertical */
    check(gol_get(&g, 2, 1) && gol_get(&g, 2, 2) && gol_get(&g, 2, 3),
          "blinker gen2 vertical again");
    check_eq(gol_live_count(&g), 3, "blinker stays 3 cells");
    gol_free(&g);
}

/* A 2x2 block is a still life. */
static void test_block(void) {
    gol_settings_t cfg = {.trail_turns = 1};
    gol_t g;
    gol_init(&g, 6, 6, &cfg);
    gol_set(&g, 2, 2, true);
    gol_set(&g, 3, 2, true);
    gol_set(&g, 2, 3, true);
    gol_set(&g, 3, 3, true);

    for (int i = 0; i < 3; i++)
        gol_step(&g);
    check(gol_get(&g, 2, 2) && gol_get(&g, 3, 2) && gol_get(&g, 2, 3) &&
              gol_get(&g, 3, 3),
          "block survives");
    check_eq(gol_live_count(&g), 4, "block stays 4 cells");
    gol_free(&g);
}

/* A live cell at corner (0,0) with two neighbours wrapping across the opposite
 * edges should come alive at the wrapped corner via toroidal neighbour count. */
static void test_toroidal_corner(void) {
    gol_settings_t cfg = {.trail_turns = 1};
    gol_t g;
    gol_init(&g, 4, 4, &cfg);
    /* Three live cells whose shared neighbour wraps to corner (0,0):
     * place an L that births a cell at the opposite corner. Use cells at
     * (0,0),(3,0),(0,3) — each pair are toroidal neighbours of corner (3,3). */
    gol_set(&g, 0, 0, true);
    gol_set(&g, 3, 0, true);
    gol_set(&g, 0, 3, true);
    /* Corner (3,3) has neighbours (incl. wrap): (0,0),(3,0),(0,3) all alive -> 3
     * live neighbours -> birth. */
    gol_step(&g);
    check(gol_get(&g, 3, 3), "toroidal birth at wrapped corner");
    gol_free(&g);
}

/* All-dead grid stays all-dead. */
static void test_empty_stays_empty(void) {
    gol_settings_t cfg = {.trail_turns = 1};
    gol_t g;
    gol_init(&g, 8, 8, &cfg);
    gol_step(&g);
    check_eq(gol_live_count(&g), 0, "empty grid stays empty");
    gol_free(&g);
}

/* Density 0 yields no cells; density 1 yields all cells. */
static void test_seed_density_bounds(void) {
    uint32_t rng = 12345;

    gol_settings_t zero = {.density = 0.0, .trail_turns = 1};
    gol_t g0;
    gol_init(&g0, 20, 20, &zero);
    gol_seed(&g0, &rng);
    check_eq(gol_live_count(&g0), 0, "density 0 -> no cells");
    gol_free(&g0);

    gol_settings_t full = {.density = 1.0, .trail_turns = 1};
    gol_t g1;
    gol_init(&g1, 20, 20, &full);
    gol_seed(&g1, &rng);
    check_eq(gol_live_count(&g1), 400, "density 1 -> all cells");
    gol_free(&g1);
}

/* Density ~0.33 yields roughly a third of cells within tolerance. */
static void test_seed_density_third(void) {
    uint32_t rng = 0xC0FFEE;
    gol_settings_t cfg = {.density = 0.33, .trail_turns = 1};
    gol_t g;
    gol_init(&g, 100, 100, &cfg); /* 10000 cells */
    gol_seed(&g, &rng);
    int live = gol_live_count(&g);
    check(live > 3000 && live < 3600, "density 0.33 ~ a third (within tolerance)");
    gol_free(&g);
}

/* Trail off: a dead cell is immediately dark. Trail on: it fades over turns. */
static void test_trail(void) {
    /* Trail off */
    gol_settings_t off = {.trail = false, .trail_turns = 3};
    gol_t go;
    gol_init(&go, 5, 5, &off);
    gol_set(&go, 2, 2, true); /* lone cell dies next step */
    gol_step(&go);
    check(!gol_get(&go, 2, 2), "lone cell died");
    check_eq(gol_trail(&go, 2, 2), 0, "trail off -> immediately dark");
    gol_free(&go);

    /* Trail on, trail_turns = 3: fade 2 -> 1 -> 0 over three dead steps */
    gol_settings_t on = {.trail = true, .trail_turns = 3};
    gol_t gt;
    gol_init(&gt, 5, 5, &on);
    gol_set(&gt, 2, 2, true);
    gol_step(&gt); /* died this step */
    check(!gol_get(&gt, 2, 2), "trail: lone cell died");
    check_eq(gol_trail(&gt, 2, 2), 2, "trail step1 = 2");
    gol_step(&gt);
    check_eq(gol_trail(&gt, 2, 2), 1, "trail step2 = 1");
    gol_step(&gt);
    check_eq(gol_trail(&gt, 2, 2), 0, "trail step3 = 0 (faded)");
    gol_free(&gt);
}

/* The legacy single-board pixel formula, reproduced here so the new compose
 * path can be proven byte-identical to the pre-rgb renderer. */
static uint32_t legacy_pixel(const gol_t *g, int x, int y) {
    int tt = g->cfg.trail_turns > 0 ? g->cfg.trail_turns : 1;
    if (gol_get(g, x, y))
        return 0xFF00FF00u; /* live: full green */
    uint8_t t = gol_trail(g, x, y);
    if (t == 0)
        return 0xFF000000u; /* dark */
    uint32_t green = (uint32_t)(255 * t / tt);
    return 0xFF000000u | (green << 8);
}

/* Channel intensity: full when alive, faded for a trail, dark otherwise. */
static void test_channel_intensity(void) {
    check_eq(gol_channel_intensity(true, 0, 8), 255, "alive -> 255");
    check_eq(gol_channel_intensity(false, 0, 8), 0, "dead, no trail -> 0");
    check_eq(gol_channel_intensity(false, 8, 8), 255, "full trail -> 255");
    check_eq(gol_channel_intensity(false, 4, 8), 127, "half trail -> 127");
    check_eq(gol_channel_intensity(false, 1, 0), 255, "trail_turns<1 clamps to 1");
}

/* Compose: single board is green-only; rgb maps channels and combines colors. */
static void test_compose_pixel(void) {
    check(gol_compose_pixel(255, 0, 0, 1) == 0xFF00FF00u,
          "single live -> green");
    check(gol_compose_pixel(128, 0, 0, 1) == (0xFF000000u | (128u << 8)),
          "single trail -> faded green");
    check(gol_compose_pixel(0, 0, 0, 1) == 0xFF000000u, "single dark -> black");
    check(gol_compose_pixel(0, 0, 0, 3) == 0xFF000000u, "rgb all dark -> black");
    check(gol_compose_pixel(255, 0, 0, 3) == 0xFFFF0000u, "rgb board0 -> red");
    check(gol_compose_pixel(0, 255, 0, 3) == 0xFF00FF00u, "rgb board1 -> green");
    check(gol_compose_pixel(0, 0, 255, 3) == 0xFF0000FFu, "rgb board2 -> blue");
    check(gol_compose_pixel(255, 255, 0, 3) == 0xFFFFFF00u, "rgb r+g -> yellow");
    check(gol_compose_pixel(255, 255, 255, 3) == 0xFFFFFFFFu,
          "rgb all alive -> white");
}

/* The rgb-off compose path must reproduce the legacy pixel for every cell
 * across several generations (regression guard for the render refactor). */
static void test_rgb_off_parity(void) {
    gol_settings_t cfg = {.density = 0.35, .trail = true, .trail_turns = 6};
    gol_t g;
    gol_init(&g, 40, 12, &cfg);
    uint32_t rng = 0x5EED;
    gol_seed(&g, &rng);
    int tt = cfg.trail_turns;
    bool mismatch = false;
    for (int gen = 0; gen < 8 && !mismatch; gen++) {
        for (int y = 0; y < g.rows && !mismatch; y++) {
            for (int x = 0; x < g.cols; x++) {
                uint32_t legacy = legacy_pixel(&g, x, y);
                uint8_t c0 = gol_channel_intensity(gol_get(&g, x, y),
                                                   gol_trail(&g, x, y), tt);
                if (gol_compose_pixel(c0, 0, 0, 1) != legacy) {
                    mismatch = true;
                    break;
                }
            }
        }
        gol_step(&g);
    }
    check(!mismatch, "rgb-off compose is pixel-identical to legacy");
    gol_free(&g);
}

/* Two boards seeded from the same advancing rng get distinct populations and
 * step independently (one board's step does not touch the other's cells). */
static void test_rgb_board_independence(void) {
    gol_settings_t cfg = {.density = 0.4, .trail = false, .trail_turns = 1};
    gol_t a, b;
    gol_init(&a, 30, 10, &cfg);
    gol_init(&b, 30, 10, &cfg);
    uint32_t rng = 0xABCDEF;
    gol_seed(&a, &rng);
    gol_seed(&b, &rng); /* drawn from the advanced stream -> different cells */

    bool differ = false;
    for (int y = 0; y < a.rows && !differ; y++)
        for (int x = 0; x < a.cols; x++)
            if (gol_get(&a, x, y) != gol_get(&b, x, y)) {
                differ = true;
                break;
            }
    check(differ, "independent seeds yield different populations");

    int b_live = gol_live_count(&b);
    gol_step(&a);
    check_eq(gol_live_count(&b), b_live, "stepping board a leaves board b intact");
    gol_free(&a);
    gol_free(&b);
}

/* FNV-1a 64-bit matches the published test vectors; distinct inputs differ. */
static void test_fnv1a(void) {
    check(gol_fnv1a64("", 0) == 0xcbf29ce484222325ull, "fnv1a(\"\") = basis");
    check(gol_fnv1a64("a", 1) == 0xaf63dc4c8601ec8cull, "fnv1a(\"a\") vector");
    check(gol_fnv1a64("a", 1) != gol_fnv1a64("b", 1), "fnv1a distinguishes inputs");
}

/* A period-2 blinker returns to a prior state and is detected within the ring. */
static void test_cycle_blinker(void) {
    gol_settings_t cfg = {.trail_turns = 1};
    gol_t g;
    gol_init(&g, 6, 6, &cfg);
    gol_set(&g, 1, 2, true);
    gol_set(&g, 2, 2, true);
    gol_set(&g, 3, 2, true);
    gol_cycle_t c;
    gol_cycle_reset(&c);
    bool detected = false;
    for (int i = 0; i < 6 && !detected; i++) {
        gol_step(&g);
        detected = gol_cycle_record(&c, &g);
    }
    check(detected, "blinker cycle detected");
    gol_free(&g);
}

/* A 2x2 block is a still life: the next state repeats immediately. */
static void test_cycle_block(void) {
    gol_settings_t cfg = {.trail_turns = 1};
    gol_t g;
    gol_init(&g, 6, 6, &cfg);
    gol_set(&g, 2, 2, true);
    gol_set(&g, 3, 2, true);
    gol_set(&g, 2, 3, true);
    gol_set(&g, 3, 3, true);
    gol_cycle_t c;
    gol_cycle_reset(&c);
    gol_step(&g);
    gol_cycle_record(&c, &g); /* store the (unchanged) block */
    gol_step(&g);
    check(gol_cycle_record(&c, &g), "still-life detected");
    gol_free(&g);
}

/* A glider translates across a large board and never repeats within 16 gens. */
static void test_cycle_glider(void) {
    gol_settings_t cfg = {.trail_turns = 1};
    gol_t g;
    gol_init(&g, 40, 40, &cfg);
    gol_set(&g, 1, 0, true);
    gol_set(&g, 2, 1, true);
    gol_set(&g, 0, 2, true);
    gol_set(&g, 1, 2, true);
    gol_set(&g, 2, 2, true);
    gol_cycle_t c;
    gol_cycle_reset(&c);
    bool detected = false;
    for (int i = 0; i < 16 && !detected; i++) {
        gol_step(&g);
        detected = gol_cycle_record(&c, &g);
    }
    check(!detected, "glider not detected within 16 gens");
    gol_free(&g);
}

/* Resetting the ring clears prior hashes: a repeat after reset is not matched. */
static void test_cycle_reset(void) {
    gol_settings_t cfg = {.trail_turns = 1};
    gol_t g;
    gol_init(&g, 6, 6, &cfg);
    gol_set(&g, 2, 2, true);
    gol_set(&g, 3, 2, true);
    gol_set(&g, 2, 3, true);
    gol_set(&g, 3, 3, true);
    gol_cycle_t c;
    gol_cycle_reset(&c);
    gol_step(&g);
    gol_cycle_record(&c, &g); /* store block hash */
    gol_cycle_reset(&c);      /* simulate reseed clearing the ring */
    gol_step(&g);
    check(!gol_cycle_record(&c, &g), "cleared ring does not match prior run");
    gol_free(&g);
}

/* With counter > 16, the oldest slot is evicted; an evicted hash no longer
 * matches while a still-live one does. */
static void test_cycle_ring_wrap(void) {
    gol_settings_t cfg = {.trail_turns = 1};
    gol_t g;
    gol_init(&g, 20, 2, &cfg); /* 40 cells: room for 17 distinct one-cell boards */
    gol_cycle_t c;
    gol_cycle_reset(&c);
    /* Record 17 distinct boards (only cell i alive). Pattern 0 is stored at
     * slot 0 (counter 0), then overwritten when counter reaches 16. */
    for (int i = 0; i < 17; i++) {
        gol_clear(&g);
        gol_set(&g, i, 0, true);
        gol_cycle_record(&c, &g);
    }
    gol_clear(&g);
    gol_set(&g, 0, 0, true);
    check(!gol_cycle_record(&c, &g), "evicted hash no longer detected");
    gol_clear(&g);
    gol_set(&g, 5, 0, true); /* still inside the live 16-slot window */
    check(gol_cycle_record(&c, &g), "in-window hash still detected");
    gol_free(&g);
}

int main(void) {
    test_blinker();
    test_block();
    test_toroidal_corner();
    test_empty_stays_empty();
    test_seed_density_bounds();
    test_seed_density_third();
    test_trail();
    test_channel_intensity();
    test_compose_pixel();
    test_rgb_off_parity();
    test_rgb_board_independence();
    test_fnv1a();
    test_cycle_blinker();
    test_cycle_block();
    test_cycle_glider();
    test_cycle_reset();
    test_cycle_ring_wrap();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_gol: all passed\n");
    return 0;
}
