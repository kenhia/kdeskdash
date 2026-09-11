/**
 * @file test_claude_view.c
 * Host-only unit tests for the claude mode's panel-side rendering helpers.
 *
 * The feed's own contract moved to libkdash in sprint 034 and is tested there;
 * what is tested here is the part CD-10 keeps on the panel. The one exception
 * is test_library_ladder_contract() — see its comment: this panel renders a
 * derivation it no longer owns, so it asserts the behaviour it depends on
 * rather than assuming it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kdash/kdash_freshness.h"
#include "kdash/kdash_payload.h"
#include "modes/claude_view.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void check_str(const char *got, const char *want, const char *what) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL: %s — got \"%s\", want \"%s\"\n", what, got, want);
        failures++;
    }
}

/* ---------- display labels ---------- */

static void test_disp_label(void) {
    check_str(claude_disp_label(KDASH_CLAUDE_DISP_BLOCKED), "BLOCKED ON YOU",
              "blocked label");
    check_str(claude_disp_label(KDASH_CLAUDE_DISP_AWAITING), "AWAITING INPUT",
              "awaiting label");
    check_str(claude_disp_label(KDASH_CLAUDE_DISP_WORKING), "WORKING",
              "working label");
    check_str(claude_disp_label(KDASH_CLAUDE_DISP_IDLE), "IDLE", "idle label");
    check_str(claude_disp_label(KDASH_CLAUDE_DISP_STALE), "STALE", "stale label");

    /* The view clips the status label at a fixed width that AWAITING INPUT was
     * sized against; keep the widest label no wider so it cannot overflow. */
    check(strlen(claude_disp_label(KDASH_CLAUDE_DISP_BLOCKED)) <=
              strlen(claude_disp_label(KDASH_CLAUDE_DISP_AWAITING)),
          "blocked label fits awaiting's width");
}

/* ---------- the absent-project placeholder ---------- */

static void test_project_label(void) {
    check_str(claude_project_label("kdeskdash"), "kdeskdash", "project passes through");
    /* libkdash leaves an absent project as "" on purpose — the placeholder is
     * rendering, so it lives here (CD-10). */
    check_str(claude_project_label(""), "?", "empty project becomes ?");
    check_str(claude_project_label(NULL), "?", "NULL project becomes ?");
}

/* ---------- formatting ---------- */

static void test_fmt(void) {
    char buf[32];
    claude_fmt_age(12, buf, sizeof(buf));     check_str(buf, "12s", "age 12s");
    claude_fmt_age(185, buf, sizeof(buf));    check_str(buf, "3m", "age 3m");
    claude_fmt_age(7200, buf, sizeof(buf));   check_str(buf, "2h", "age 2h");
    claude_fmt_age(200000, buf, sizeof(buf)); check_str(buf, "2d", "age 2d");
    claude_fmt_age(-5, buf, sizeof(buf));     check_str(buf, "0s", "negative clamps");

    /* Reset formatting is TZ-dependent: pin UTC for determinism. */
    setenv("TZ", "UTC", 1);
    tzset();
    long long now = 1783036800; /* 2026-07-03 00:00:00 UTC */
    claude_fmt_reset(now + 2 * 3600, now, buf, sizeof(buf));
    check_str(buf, "02:00", "near reset is bare clock");
    claude_fmt_reset(now + 30 * 3600, now, buf, sizeof(buf));
    check_str(buf, "Sat 06:00", "far reset carries weekday");
    claude_fmt_reset(0, now, buf, sizeof(buf));
    check_str(buf, "--", "unknown reset");
}

/* ---------- the contract this panel now depends on ---------- */

/* Not a re-derivation of libkdash's ladder — an assertion about it. This mode
 * renders a display state it no longer computes, and the one behaviour its
 * layout is built around is that `blocked` and `awaiting` stay prominent
 * through the idle band while `working` degrades: the row wash and the
 * attention-first order both assume it. libkdash tests its own ladder; this
 * says out loud which part of it kdeskdash is standing on, so a change there
 * fails here rather than quietly changing what the panel shows. */
static void test_library_ladder_contract(void) {
    const long long idle = KDASH_CLAUDE_IDLE_S, stale = KDASH_CLAUDE_STALE_S;

    check(kdash_claude_display(KDASH_CLAUDE_WORKING, 5, idle, stale) ==
              KDASH_CLAUDE_DISP_WORKING, "fresh working");
    check(kdash_claude_display(KDASH_CLAUDE_WORKING, idle, idle, stale) ==
              KDASH_CLAUDE_DISP_IDLE, "working degrades at idle");
    check(kdash_claude_display(KDASH_CLAUDE_AWAITING, idle, idle, stale) ==
              KDASH_CLAUDE_DISP_AWAITING, "awaiting stays prominent past idle");
    check(kdash_claude_display(KDASH_CLAUDE_BLOCKED, idle, idle, stale) ==
              KDASH_CLAUDE_DISP_BLOCKED, "blocked stays prominent past idle");
    check(kdash_claude_display(KDASH_CLAUDE_BLOCKED, stale, idle, stale) ==
              KDASH_CLAUDE_DISP_STALE, "age wins at the stale boundary");

    /* The enum order IS the sort rank the AGENTS zone renders in. */
    check(KDASH_CLAUDE_DISP_BLOCKED < KDASH_CLAUDE_DISP_AWAITING &&
              KDASH_CLAUDE_DISP_AWAITING < KDASH_CLAUDE_DISP_WORKING &&
              KDASH_CLAUDE_DISP_WORKING < KDASH_CLAUDE_DISP_IDLE &&
              KDASH_CLAUDE_DISP_IDLE < KDASH_CLAUDE_DISP_STALE,
          "attention-first rank order");
}

int main(void) {
    test_disp_label();
    test_project_label();
    test_fmt();
    test_library_ladder_contract();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_claude_view: all tests passed\n");
    return 0;
}
