/**
 * @file test_stopwatch.c
 * Host-only unit tests for the pure wall-clock stopwatch.
 */
#include <stdio.h>
#include <string.h>

#include "stopwatch.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void check_str(const char *got, const char *want, const char *what) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
        failures++;
    }
}

/* Start at t=0, query at t=12.34s -> 0:12.3; stop freezes; reset zeroes. */
static void test_happy_path(void) {
    stopwatch_t sw;
    stopwatch_init(&sw);
    stopwatch_start(&sw, 0);

    char buf[16];
    stopwatch_format(stopwatch_elapsed_ms(&sw, 12340), buf, sizeof(buf));
    check_str(buf, "0:12.3", "elapsed at 12.34s");

    stopwatch_stop(&sw, 12340);
    /* Frozen: later `now` must not change the elapsed value. */
    check(stopwatch_elapsed_ms(&sw, 99999) == 12340, "stop freezes elapsed");

    stopwatch_reset(&sw, 12340);
    stopwatch_format(stopwatch_elapsed_ms(&sw, 50000), buf, sizeof(buf));
    check_str(buf, "0:00.0", "reset zeroes");
}

/* Reset while running zeroes elapsed but keeps counting from now. */
static void test_reset_while_running(void) {
    stopwatch_t sw;
    stopwatch_init(&sw);
    stopwatch_start(&sw, 1000);
    check(stopwatch_elapsed_ms(&sw, 6000) == 5000, "running 5s before reset");

    stopwatch_reset(&sw, 6000);
    check(sw.running, "still running after reset");
    check(stopwatch_elapsed_ms(&sw, 6000) == 0, "reset zeroes at reset instant");
    check(stopwatch_elapsed_ms(&sw, 8500) == 2500, "keeps counting after reset");

    /* Reset while stopped stays at zero and stopped. */
    stopwatch_stop(&sw, 8500);
    stopwatch_reset(&sw, 9000);
    check(!sw.running, "stopped reset stays stopped");
    check(stopwatch_elapsed_ms(&sw, 99999) == 0, "stopped reset stays zero");
}

/* Start/stop/start accumulates across pauses; querying while stopped is frozen. */
static void test_accumulate_across_pauses(void) {
    stopwatch_t sw;
    stopwatch_init(&sw);
    stopwatch_start(&sw, 1000);
    stopwatch_stop(&sw, 4000); /* banked 3000 */
    /* While stopped, elapsed is frozen regardless of now. */
    check(stopwatch_elapsed_ms(&sw, 10000) == 3000, "frozen while stopped");

    stopwatch_start(&sw, 8000); /* resume */
    check(stopwatch_elapsed_ms(&sw, 8500) == 3500, "accumulates after resume");
    stopwatch_stop(&sw, 9000);
    check(stopwatch_elapsed_ms(&sw, 20000) == 4000, "banked across two segments");
}

/* Formatting boundaries: 59.9s, 60.0s, sub-0.1s rounding stable (truncates). */
static void test_format_boundaries(void) {
    char buf[16];
    stopwatch_format(59900, buf, sizeof(buf));
    check_str(buf, "0:59.9", "59.9s");
    stopwatch_format(60000, buf, sizeof(buf));
    check_str(buf, "1:00.0", "60.0s");
    stopwatch_format(59999, buf, sizeof(buf));
    check_str(buf, "0:59.9", "sub-0.1s truncates down");
    stopwatch_format(0, buf, sizeof(buf));
    check_str(buf, "0:00.0", "zero");
    stopwatch_format(605400, buf, sizeof(buf));
    check_str(buf, "10:05.4", "ten minutes");
}

/* Toggle alternates start/stop. */
static void test_toggle(void) {
    stopwatch_t sw;
    stopwatch_init(&sw);
    stopwatch_toggle(&sw, 100); /* start */
    check(sw.running, "toggle starts");
    stopwatch_toggle(&sw, 600); /* stop, banked 500 */
    check(!sw.running, "toggle stops");
    check(stopwatch_elapsed_ms(&sw, 9999) == 500, "toggle banked 500ms");
}

int main(void) {
    test_happy_path();
    test_reset_while_running();
    test_accumulate_across_pauses();
    test_format_boundaries();
    test_toggle();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_stopwatch: all passed\n");
    return 0;
}
