/**
 * @file test_clock_core.c
 * Host-only unit tests for the pure clock core: face formatting either side of
 * a DST boundary and across a date line, and the type-scale thresholds the
 * shared widget picks its fonts from.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clock_core.h"

static int failures;

static void check(long got, long want, const char *what) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %ld, want %ld\n", what, got, want);
        failures++;
    }
}

static void check_str(const char *got, const char *want, const char *what) {
    if (!got || strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s: got \"%s\", want \"%s\"\n", what,
                got ? got : "(null)", want);
        failures++;
    }
}

static void pin_tz(const char *tz) {
    setenv("TZ", tz, 1);
    tzset();
}

/* 2026-07-18 23:09:59 UTC == 16:09:59 PDT — the timestamp from the frozen
 * kvscf contract's example payload. */
#define T_SUMMER ((time_t)1784416199)
/* 2026-01-01 00:00:00 UTC == 2025-12-31 16:00:00 PST — standard time, and the
 * two faces are a *day* apart, which is the whole point of showing both. */
#define T_WINTER ((time_t)1767225600)

static void test_local_face(void) {
    pin_tz("America/Los_Angeles");
    kd_clock_face_t f;

    kd_clock_local_face(T_SUMMER, &f);
    check_str(f.hm, "16:09", "local HH:MM (PDT)");
    check_str(f.sec, ":59", "local seconds");
    check_str(f.date, "Sat 18 Jul", "local date");
    check_str(f.zone, "PDT", "daylight zone abbreviation");

    kd_clock_local_face(T_WINTER, &f);
    check_str(f.hm, "16:00", "local HH:MM (PST)");
    check_str(f.date, "Wed 31 Dec", "local date, previous year");
    check_str(f.zone, "PST", "standard zone abbreviation");
}

static void test_utc_face(void) {
    pin_tz("America/Los_Angeles"); /* the UTC face must ignore the process TZ */
    kd_clock_face_t f;

    kd_clock_utc_face(T_SUMMER, &f);
    check_str(f.hm, "23:09", "UTC HH:MM");
    check_str(f.sec, ":59", "UTC seconds");
    check_str(f.date, "Sat 18 Jul", "UTC date");
    check_str(f.zone, "UTC", "zone named UTC, not glibc's GMT");

    /* Same instant, different day on each face. */
    kd_clock_utc_face(T_WINTER, &f);
    check_str(f.hm, "00:00", "UTC midnight");
    check_str(f.date, "Thu 1 Jan", "UTC date, next year — no leading zero");
}

/* The faces must not depend on the ambient TZ for UTC, nor leak state. */
static void test_tz_independence(void) {
    kd_clock_face_t a, b;
    pin_tz("Europe/London");
    kd_clock_utc_face(T_SUMMER, &a);
    pin_tz("Asia/Tokyo");
    kd_clock_utc_face(T_SUMMER, &b);
    check_str(a.hm, b.hm, "UTC face identical under different process TZ");

    pin_tz("Asia/Tokyo");
    kd_clock_local_face(T_SUMMER, &a);
    check_str(a.hm, "08:09", "local face follows the process TZ (JST)");
    check_str(a.date, "Sun 19 Jul", "and its date");
}

static void test_tier(void) {
    /* The two real call sites. The Launcher pane earns the large scale: it has
     * far more room than the big face needs, and the first calibration
     * (900 px of width) wrongly demoted it — see clock_core.c. */
    check(kd_clock_tier(576, 440), KD_CLOCK_TIER_L, "Launcher side pane -> L");
    check(kd_clock_tier(1920, 440), KD_CLOCK_TIER_L, "full-screen clock -> L");

    /* Both dimensions bind — neither a tall sliver nor a short banner gets L. */
    check(kd_clock_tier(1920, 379), KD_CLOCK_TIER_M, "too short for L");
    check(kd_clock_tier(519, 440), KD_CLOCK_TIER_M, "too narrow for L");
    check(kd_clock_tier(419, 440), KD_CLOCK_TIER_S, "too narrow for M");
    check(kd_clock_tier(576, 299), KD_CLOCK_TIER_S, "too short for M");
    check(kd_clock_tier(299, 440), KD_CLOCK_TIER_XS, "too narrow for S");
    check(kd_clock_tier(576, 199), KD_CLOCK_TIER_XS, "too short for S");

    /* Exact boundaries land in the larger tier. */
    check(kd_clock_tier(520, 380), KD_CLOCK_TIER_L, "L boundary inclusive");
    check(kd_clock_tier(420, 300), KD_CLOCK_TIER_M, "M boundary inclusive");
    check(kd_clock_tier(300, 200), KD_CLOCK_TIER_S, "S boundary inclusive");

    /* Degenerate sizes (a pane asked before layout) must not go out of range. */
    check(kd_clock_tier(0, 0), KD_CLOCK_TIER_XS, "zero -> XS");
    check(kd_clock_tier(-10, -10), KD_CLOCK_TIER_XS, "negative -> XS");
}

int main(void) {
    test_local_face();
    test_utc_face();
    test_tz_independence();
    test_tier();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_clock_core: all passed\n");
    return 0;
}
