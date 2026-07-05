/**
 * @file test_settings.c
 * Host-only unit tests for gol_settings_apply_field() — the untrusted-input
 * validation boundary extracted from redis.c. Boundary values on every field,
 * plus the cell_size floor that drifted once (the regression this guards), and
 * the stricter non-numeric handling (rejected, not coerced to 0).
 */
#include <stdio.h>

#include "gol_settings.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* A cfg pre-filled with sentinels distinct from every value the tests inject,
 * so "unchanged" is unambiguous. */
static gol_settings_t sentinel_cfg(void) {
    gol_settings_t c = {
        .cell_size = 7,
        .padding = 7,
        .density = 0.42,
        .trail = false,
        .trail_turns = 7,
        .speed_ms = 777,
        .rgb = false,
    };
    return c;
}

static void test_cell_size(void) {
    gol_settings_t c = sentinel_cfg();
    check(!gol_settings_apply_field(&c, "cell_size", "1"), "cell_size 1 rejected (floor is 2)");
    check(c.cell_size == 7, "cell_size 1 leaves default");
    check(gol_settings_apply_field(&c, "cell_size", "2"), "cell_size 2 accepted (floor)");
    check(c.cell_size == 2, "cell_size 2 applied");
    check(gol_settings_apply_field(&c, "cell_size", "64"), "cell_size 64 accepted (ceil)");
    check(c.cell_size == 64, "cell_size 64 applied");
    check(!gol_settings_apply_field(&c, "cell_size", "65"), "cell_size 65 rejected");
    check(c.cell_size == 64, "cell_size 65 leaves prior");
}

static void test_padding(void) {
    gol_settings_t c = sentinel_cfg();
    check(!gol_settings_apply_field(&c, "padding", "-1"), "padding -1 rejected");
    check(c.padding == 7, "padding -1 leaves default");
    check(gol_settings_apply_field(&c, "padding", "0"), "padding 0 accepted (floor)");
    check(c.padding == 0, "padding 0 applied");
    check(gol_settings_apply_field(&c, "padding", "16"), "padding 16 accepted (ceil)");
    check(c.padding == 16, "padding 16 applied");
    check(!gol_settings_apply_field(&c, "padding", "17"), "padding 17 rejected");
}

static void test_density(void) {
    gol_settings_t c = sentinel_cfg();
    check(!gol_settings_apply_field(&c, "density", "0.0"), "density 0.0 rejected (exclusive floor)");
    check(c.density == 0.42, "density 0.0 leaves default");
    check(gol_settings_apply_field(&c, "density", "0.001"), "density just-above-0 accepted");
    check(c.density > 0.0 && c.density < 0.01, "density eps applied");
    check(gol_settings_apply_field(&c, "density", "1.0"), "density 1.0 accepted (ceil)");
    check(c.density == 1.0, "density 1.0 applied");
    check(!gol_settings_apply_field(&c, "density", "1.5"), "density >1.0 rejected");
    check(c.density == 1.0, "density 1.5 leaves prior");
}

static void test_trail_turns(void) {
    gol_settings_t c = sentinel_cfg();
    check(!gol_settings_apply_field(&c, "trail_turns", "0"), "trail_turns 0 rejected");
    check(gol_settings_apply_field(&c, "trail_turns", "1"), "trail_turns 1 accepted (floor)");
    check(c.trail_turns == 1, "trail_turns 1 applied");
    check(gol_settings_apply_field(&c, "trail_turns", "64"), "trail_turns 64 accepted (ceil)");
    check(!gol_settings_apply_field(&c, "trail_turns", "65"), "trail_turns 65 rejected");
}

static void test_speed_ms(void) {
    gol_settings_t c = sentinel_cfg();
    check(!gol_settings_apply_field(&c, "speed_ms", "9"), "speed_ms 9 rejected");
    check(gol_settings_apply_field(&c, "speed_ms", "10"), "speed_ms 10 accepted (floor)");
    check(c.speed_ms == 10, "speed_ms 10 applied");
    check(gol_settings_apply_field(&c, "speed_ms", "5000"), "speed_ms 5000 accepted (ceil)");
    check(!gol_settings_apply_field(&c, "speed_ms", "5001"), "speed_ms 5001 rejected");
}

static void test_bool_fields(void) {
    gol_settings_t c = sentinel_cfg();
    check(gol_settings_apply_field(&c, "trail", "1"), "trail 1 accepted");
    check(c.trail == true, "trail 1 -> true");
    check(gol_settings_apply_field(&c, "trail", "0"), "trail 0 accepted");
    check(c.trail == false, "trail 0 -> false");
    check(gol_settings_apply_field(&c, "trail", "5"), "trail nonzero accepted");
    check(c.trail == true, "trail 5 -> true (nonzero)");
    check(gol_settings_apply_field(&c, "rgb", "1"), "rgb 1 accepted");
    check(c.rgb == true, "rgb 1 -> true");
}

static void test_unknown_and_junk(void) {
    gol_settings_t c = sentinel_cfg();
    check(!gol_settings_apply_field(&c, "not_a_field", "3"), "unknown field is no-op");

    /* Non-numeric: strictly rejected now (old atoi() would have coerced to 0). */
    check(!gol_settings_apply_field(&c, "cell_size", "abc"), "non-numeric cell_size rejected");
    check(c.cell_size == 7, "non-numeric leaves default (not coerced to 0)");
    check(!gol_settings_apply_field(&c, "cell_size", ""), "empty value rejected");
    check(!gol_settings_apply_field(&c, "cell_size", "3.9"), "trailing junk (3.9) rejected for int field");
    check(!gol_settings_apply_field(&c, "cell_size", "12px"), "trailing junk (12px) rejected");
    check(!gol_settings_apply_field(&c, "cell_size", "99999999999999999999"), "overflow rejected");
    check(c.cell_size == 7, "overflow leaves default");

    /* NULL guards. */
    check(!gol_settings_apply_field(NULL, "cell_size", "3"), "NULL cfg rejected");
    check(!gol_settings_apply_field(&c, NULL, "3"), "NULL field rejected");
    check(!gol_settings_apply_field(&c, "cell_size", NULL), "NULL val rejected");
}

int main(void) {
    test_cell_size();
    test_padding();
    test_density();
    test_trail_turns();
    test_speed_ms();
    test_bool_fields();
    test_unknown_and_junk();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_settings: all passed\n");
    return 0;
}
