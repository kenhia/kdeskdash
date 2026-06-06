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

int main(void) {
    test_blinker();
    test_block();
    test_toroidal_corner();
    test_empty_stays_empty();
    test_seed_density_bounds();
    test_seed_density_third();
    test_trail();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_gol: all passed\n");
    return 0;
}
