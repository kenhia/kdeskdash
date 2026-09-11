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


/* --- sprint 036: WI #1136, the almanac and the extra zone faces ---------- */

static void test_zone_face(void) {
    pin_tz("America/Los_Angeles");
    kd_clock_face_t f;

    /* T_SUMMER is 23:09:59 UTC; Tokyo is UTC+9 year round. */
    kd_clock_zone_face(T_SUMMER, "Asia/Tokyo", &f);
    check_str(f.hm, "08:09", "Tokyo is nine hours ahead of UTC");
    check_str(f.date, "Sun 19 Jul", "and already on the next day");
    check_str(f.zone, "JST", "zone abbreviation comes from the zone");

    /* The process timezone must survive the excursion — the local face is
     * formatted from it, and a leaked TZ would silently relabel the panel. */
    kd_clock_face_t local;
    kd_clock_local_face(T_SUMMER, &local);
    check_str(local.hm, "16:09", "process TZ restored after a zone face");
    check_str(local.zone, "PDT", "and its abbreviation with it");

    /* Same, starting from no TZ at all: the unset case must be restored as
     * unset rather than as the zone we borrowed. */
    unsetenv("TZ");
    tzset();
    kd_clock_zone_face(T_SUMMER, "Asia/Tokyo", &f);
    check(getenv("TZ") == NULL, 1, "an unset TZ is restored as unset");

    /* Degenerate zones are empty faces, not crashes. */
    pin_tz("America/Los_Angeles");
    kd_clock_zone_face(T_SUMMER, NULL, &f);
    check_str(f.hm, "", "a NULL zone yields an empty face");
    kd_clock_zone_face(T_SUMMER, "", &f);
    check_str(f.hm, "", "an empty zone yields an empty face");
    kd_clock_local_face(T_SUMMER, &local);
    check_str(local.hm, "16:09", "and neither disturbs the process TZ");
}

static void test_almanac(void) {
    pin_tz("America/Los_Angeles");
    kd_clock_almanac_t a;

    /* T_SUMMER is 2026-07-18 16:09:59 PDT — a Saturday. */
    kd_clock_almanac(T_SUMMER, &a);
    check_str(a.long_date, "Saturday 18 July 2026", "long date spells it out");
    check(a.yday, 199, "18 July 2026 is day 199");
    check(a.ydays, 365, "2026 is not a leap year");
    check(a.iso_week, 29, "and falls in ISO week 29");

    /* T_WINTER is 2025-12-31 16:00 PST locally — the almanac follows the local
     * date, not UTC's, so this is still the 31st and still 2025. */
    kd_clock_almanac(T_WINTER, &a);
    check_str(a.long_date, "Wednesday 31 December 2025",
              "the almanac uses the local date");
    check(a.yday, 365, "31 December 2025 is day 365");
    check(a.ydays, 365, "2025 has 365 days");
    /* ISO week rolls into the next year's week 1 at the end of December. */
    check(a.iso_week, 1, "31 Dec 2025 is ISO week 1 of 2026");

    /* A leap year counts its extra day. 2024-02-29 12:00 UTC. */
    pin_tz("UTC");
    kd_clock_almanac((time_t)1709208000, &a);
    check_str(a.long_date, "Thursday 29 February 2024", "leap day formats");
    check(a.yday, 60, "29 February is day 60");
    check(a.ydays, 366, "2024 is a leap year");
}

static void test_almanac_progress(void) {
    pin_tz("UTC");
    kd_clock_almanac_t a;

    /* 2026-01-01 00:00:00 UTC — the very start of a year, a day, and (it being
     * a Thursday) the middle of an ISO week. */
    kd_clock_almanac((time_t)1767225600, &a);
    check(a.day_pct, 0, "midnight is 0% through the day");
    check(a.year_pct, 0, "1 January is 0% through the year");
    /* Thursday is ISO weekday 3, so three whole days of seven. */
    check(a.week_pct, 3 * 100 / 7, "Thursday midnight is 3/7 through the week");

    /* Noon the same day: half a day gone. */
    kd_clock_almanac((time_t)(1767225600 + 12 * 3600), &a);
    check(a.day_pct, 50, "noon is 50% through the day");

    /* The last second of a day reads 99, never 100 — a bar that sits full
     * while the unit is still running is the thing being avoided. */
    kd_clock_almanac((time_t)(1767225600 + 86399), &a);
    check(a.day_pct, 99, "23:59:59 is 99% through the day");

    /* The last second of the year, likewise. 2026-12-31 23:59:59 UTC. */
    kd_clock_almanac((time_t)(1767225600 + 365 * 86400 - 1), &a);
    check(a.yday, 365, "the last day of 2026");
    check(a.year_pct, 99, "the last second of the year is 99%");

    /* Monday 00:00 restarts the week. 2026-01-05 is the Monday after. */
    kd_clock_almanac((time_t)(1767225600 + 4 * 86400), &a);
    check(a.week_pct, 0, "Monday midnight is 0% through the week");

    /* Sunday's last second is the top of the range. 2026-01-11 23:59:59. */
    kd_clock_almanac((time_t)(1767225600 + 11 * 86400 - 1), &a);
    check(a.week_pct, 99, "the last second of Sunday is 99%");

    /* Every percentage stays in range across a whole year of samples, which is
     * the property the bars actually depend on. */
    for (long d = 0; d < 365; d++) {
        for (long h = 0; h < 24; h += 7) {
            kd_clock_almanac((time_t)(1767225600 + d * 86400 + h * 3600), &a);
            if (a.day_pct < 0 || a.day_pct > 99 || a.week_pct < 0 ||
                a.week_pct > 99 || a.year_pct < 0 || a.year_pct > 99) {
                fprintf(stderr, "FAIL progress out of range at day %ld hour %ld:"
                                " %d/%d/%d\n",
                        d, h, a.day_pct, a.week_pct, a.year_pct);
                failures++;
                return;
            }
        }
    }

    /* Across a DST boundary the percentages come from the *local wall clock*,
     * which is the reading that agrees with the time shown beside them.
     *
     * 2026-03-08 is the US spring-forward, so that local day is only 23 hours
     * of real time — but at local noon the bar still reads 50%, because noon
     * is halfway between midnight and midnight however many hours the day
     * actually took. A user glancing at a clock panel reads the wall, not the
     * elapsed seconds, and a "Day 48%" beside a 12:00 would look broken. */
    pin_tz("America/Los_Angeles");
    kd_clock_almanac((time_t)1772996400, &a); /* 2026-03-08 12:00 PDT */
    check_str(a.long_date, "Sunday 8 March 2026", "spring-forward local date");
    check(a.day_pct, 50, "local noon on a DST day is still 50%");
    kd_clock_almanac((time_t)(1772996400 - 3600), &a); /* 11:00 PDT */
    check(a.day_pct, 45, "and 11:00 is 45%, by the wall clock");
}

static void check_zones(const char *spec, int want_n, const char *want_label0,
                        const char *want_tz0, const char *what) {
    kd_clock_zone_t z[KD_CLOCK_ZONES_MAX];
    int n = kd_clock_parse_zones(spec, z, KD_CLOCK_ZONES_MAX);
    if (n != want_n) {
        fprintf(stderr, "FAIL %s: got %d zones, want %d\n", what, n, want_n);
        failures++;
        return;
    }
    if (want_n > 0) {
        check_str(z[0].label, want_label0, what);
        check_str(z[0].tz, want_tz0, what);
    }
}

static void test_parse_zones(void) {
    /* The default is no extra faces, and both spellings of "unset" agree. */
    check_zones(NULL, 0, NULL, NULL, "NULL spec");
    check_zones("", 0, NULL, NULL, "empty spec");
    check_zones("   ", 0, NULL, NULL, "whitespace spec");
    check_zones(",,,", 0, NULL, NULL, "only separators");

    /* A bare zone labels itself from its last path segment. */
    check_zones("Asia/Tokyo", 1, "Tokyo", "Asia/Tokyo", "bare zone");
    check_zones("America/New_York", 1, "New York", "America/New_York",
                "underscores become spaces");
    check_zones("UTC", 1, "UTC", "UTC", "a zone with no path segment");

    /* An explicit label wins, and surrounding space is ignored. */
    check_zones("HQ=Europe/London", 1, "HQ", "Europe/London", "explicit label");
    check_zones("  HQ = Europe/London  ", 1, "HQ", "Europe/London",
                "space around the parts is ignored");

    /* Several, in order. */
    kd_clock_zone_t z[KD_CLOCK_ZONES_MAX];
    int n = kd_clock_parse_zones("Asia/Tokyo, HQ=Europe/London, UTC", z,
                                 KD_CLOCK_ZONES_MAX);
    check(n, 3, "three zones parse");
    check_str(z[0].label, "Tokyo", "first zone");
    check_str(z[1].label, "HQ", "second zone");
    check_str(z[2].label, "UTC", "third zone");

    /* Malformed entries are skipped, and the good ones around them survive —
     * a typo must cost one row, not the whole strip. */
    n = kd_clock_parse_zones("Asia/Tokyo,,=Europe/London,HQ=,UTC", z,
                             KD_CLOCK_ZONES_MAX);
    check(n, 2, "empty entries and empty halves are skipped");
    check_str(z[0].tz, "Asia/Tokyo", "the good entry before them survives");
    check_str(z[1].tz, "UTC", "and the good entry after them");

    /* Over-long parts are dropped rather than truncated into a different zone
     * name — a silently shortened "Asia/Tok" would resolve to UTC. */
    char longspec[160];
    snprintf(longspec, sizeof(longspec), "Asia/Tokyo,%0*d", 100, 0);
    n = kd_clock_parse_zones(longspec, z, KD_CLOCK_ZONES_MAX);
    check(n, 1, "an over-long zone name is dropped, not truncated");
    check_str(z[0].tz, "Asia/Tokyo", "the valid entry is kept");

    /* The cap binds, and binds on the *written* entries. */
    n = kd_clock_parse_zones("UTC,UTC,UTC,UTC,UTC,UTC", z, KD_CLOCK_ZONES_MAX);
    check(n, KD_CLOCK_ZONES_MAX, "the zone cap binds");
    n = kd_clock_parse_zones("Asia/Tokyo,UTC", z, 1);
    check(n, 1, "a caller may ask for fewer");
    check_str(z[0].tz, "Asia/Tokyo", "and gets the first");

    /* Degenerate max values do not write. */
    check(kd_clock_parse_zones("UTC", z, 0), 0, "max 0 writes nothing");
    check(kd_clock_parse_zones("UTC", NULL, KD_CLOCK_ZONES_MAX), 0,
          "a NULL out writes nothing");
}

int main(void) {
    test_local_face();
    test_utc_face();
    test_tz_independence();
    test_tier();
    test_zone_face();
    test_almanac();
    test_almanac_progress();
    test_parse_zones();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_clock_core: all passed\n");
    return 0;
}
