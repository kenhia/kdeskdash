/**
 * @file test_modeset.c
 * Host-only unit tests for the per-device mode-set core: the KDESKDASH_MODES
 * grammar, the built-in default, dedupe, unknown-id and unknown-section
 * handling, ordering, and the section query API.
 *
 * The contract these lock down is "a typo must never blank the panel": every
 * malformed input path has to degrade to something usable, never to nothing.
 */
#include <stdio.h>
#include <string.h>

#include "modeset.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* Compare the flat, declaration-ordered id list against a comma-free list. */
static void check_order(const modeset_t *ms, const char *const *want, int n,
                        const char *what) {
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: count", what);
    check(modeset_count(ms) == n, msg);
    for (int i = 0; i < n && i < modeset_count(ms); i++) {
        const char *got = modeset_at(ms, i);
        snprintf(msg, sizeof(msg), "%s: [%d] want %s got %s", what, i, want[i],
                 got ? got : "(null)");
        check(got && strcmp(got, want[i]) == 0, msg);
    }
}

static void check_section(const modeset_t *ms, int s, const char *name,
                          const char *const *want, int n, const char *what) {
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: section %d name", what, s);
    check(modeset_section_name(ms, s) &&
              strcmp(modeset_section_name(ms, s), name) == 0, msg);
    snprintf(msg, sizeof(msg), "%s: section %s size", what, name);
    check(modeset_section_size(ms, s) == n, msg);
    for (int i = 0; i < n && i < modeset_section_size(ms, s); i++) {
        const char *got = modeset_section_at(ms, s, i);
        snprintf(msg, sizeof(msg), "%s: %s[%d] want %s got %s", what, name, i,
                 want[i], got ? got : "(null)");
        check(got && strcmp(got, want[i]) == 0, msg);
    }
}

/* ---- the built-in default -------------------------------------------- */

static const char *DEFAULT_ORDER[] = {"game_of_life", "golz",       "icons",
                                      "palette",      "claude",     "foreground",
                                      "clock",        "dev",        "calc"};
static const char *DEFAULT_FUN[] = {"game_of_life", "golz", "icons", "palette"};
static const char *DEFAULT_OPS[] = {"claude", "foreground", "clock", "dev",
                                    "calc"};

static void test_default(void) {
    modeset_t ms;
    modeset_default(&ms);

    check_order(&ms, DEFAULT_ORDER, 9, "default");
    check(modeset_section_count(&ms) == 2, "default has two sections");
    check_section(&ms, 0, "fun", DEFAULT_FUN, 4, "default");
    check_section(&ms, 1, "ops", DEFAULT_OPS, 5, "default");

    /* Every id in the roster is enabled by default, and nothing else is. */
    check(modeset_enabled(&ms, "palette"), "default enables palette");
    check(modeset_enabled(&ms, "dev"), "default enables dev");
    check(!modeset_enabled(&ms, "menu"), "menu is not a content mode");
    check(!modeset_enabled(&ms, "nope"), "unknown id not enabled");

    /* The roster doubles as the known-id list. */
    check(modeset_id_known("game_of_life"), "game_of_life is a known id");
    check(modeset_id_known("calc"), "calc is a known id");
    check(!modeset_id_known("launcher"), "launcher is not a known id yet");
    check(!modeset_id_known(""), "empty string is not a known id");
    check(!modeset_id_known(NULL), "NULL is not a known id");
}

/* An unset or empty var must land exactly on the default. */
static void test_unset_is_default(void) {
    modeset_t ms;

    check(!modeset_parse(&ms, NULL), "NULL spec reports 'used the default'");
    check_order(&ms, DEFAULT_ORDER, 9, "NULL spec");

    check(!modeset_parse(&ms, ""), "empty spec reports 'used the default'");
    check_order(&ms, DEFAULT_ORDER, 9, "empty spec");

    check(!modeset_parse(&ms, "   \t "), "blank spec reports 'used the default'");
    check_order(&ms, DEFAULT_ORDER, 9, "blank spec");
}

/* ---- the grammar ------------------------------------------------------ */

static void test_basic_parse(void) {
    modeset_t ms;
    static const char *want[] = {"golz", "icons", "foreground", "clock", "calc"};
    static const char *fun[] = {"golz", "icons"};
    static const char *ops[] = {"foreground", "clock", "calc"};

    check(modeset_parse(&ms, "fun:golz,icons;ops:foreground,clock,calc"),
          "well-formed spec is used");
    check_order(&ms, want, 5, "basic");
    check(modeset_section_count(&ms) == 2, "basic has two sections");
    check_section(&ms, 0, "fun", fun, 2, "basic");
    check_section(&ms, 1, "ops", ops, 3, "basic");

    check(modeset_enabled(&ms, "golz"), "golz enabled");
    check(!modeset_enabled(&ms, "dev"), "dev not in this set");
    check(!modeset_enabled(&ms, "game_of_life"), "game_of_life not in this set");
}

/* Section order follows the spec, not the built-in order. */
static void test_section_order_follows_spec(void) {
    modeset_t ms;
    static const char *want[] = {"clock", "calc", "palette"};
    static const char *ops[] = {"clock", "calc"};
    static const char *fun[] = {"palette"};

    check(modeset_parse(&ms, "ops:clock,calc;fun:palette"), "ops-first spec used");
    check_order(&ms, want, 3, "ops-first");
    check_section(&ms, 0, "ops", ops, 2, "ops-first");
    check_section(&ms, 1, "fun", fun, 1, "ops-first");
}

/* Order within a section is the spec's order, even when it inverts the default. */
static void test_mode_order_follows_spec(void) {
    modeset_t ms;
    static const char *want[] = {"palette", "icons", "golz", "game_of_life"};

    check(modeset_parse(&ms, "fun:palette,icons,golz,game_of_life"),
          "reversed fun spec used");
    check_order(&ms, want, 4, "reversed");
    check(modeset_section_count(&ms) == 1, "one section when only fun given");
}

static void test_whitespace_tolerated(void) {
    modeset_t ms;
    static const char *want[] = {"golz", "icons", "clock"};

    check(modeset_parse(&ms, "  fun : golz , icons ; ops : clock  "),
          "spaced spec used");
    check_order(&ms, want, 3, "spaced");
    check(modeset_section_name(&ms, 0) &&
              strcmp(modeset_section_name(&ms, 0), "fun") == 0,
          "spaced section name is trimmed");
}

static void test_empty_section_is_fine(void) {
    modeset_t ms;
    static const char *want[] = {"clock"};
    static const char *ops[] = {"clock"};

    check(modeset_parse(&ms, "fun:;ops:clock"), "empty fun section used");
    check_order(&ms, want, 1, "empty-fun");
    check(modeset_section_count(&ms) == 2, "empty section is still a section");
    check(modeset_section_size(&ms, 0) == 0, "empty fun has no modes");
    check(modeset_section_at(&ms, 0, 0) == NULL, "empty fun indexes to NULL");
    check_section(&ms, 1, "ops", ops, 1, "empty-fun");

    /* Trailing separators and stray commas must not invent entries. */
    check(modeset_parse(&ms, "fun:golz,;ops:clock,,"), "stray commas used");
    check(modeset_count(&ms) == 2, "stray commas add no modes");
}

/* ---- degradation ------------------------------------------------------ */

static void test_unknown_id_skipped(void) {
    modeset_t ms;
    static const char *want[] = {"golz", "clock"};

    fprintf(stderr, "-- expect a warning about 'nosuchmode' --\n");
    check(modeset_parse(&ms, "fun:golz,nosuchmode;ops:clock"),
          "spec with one bad id is still used");
    check_order(&ms, want, 2, "unknown-id");
    check(!modeset_enabled(&ms, "nosuchmode"), "unknown id is not enabled");
}

static void test_unknown_section_skipped(void) {
    modeset_t ms;
    static const char *want[] = {"clock"};

    fprintf(stderr, "-- expect a warning about section 'games' --\n");
    check(modeset_parse(&ms, "games:golz;ops:clock"),
          "spec with one bad section is still used");
    check_order(&ms, want, 1, "unknown-section");
    check(modeset_section_count(&ms) == 1, "bad section is dropped whole");
    check(!modeset_enabled(&ms, "golz"), "modes under a bad section are dropped");
}

/* A section with no ':' is malformed — dropped, not guessed at. */
static void test_missing_colon_skipped(void) {
    modeset_t ms;
    static const char *want[] = {"clock"};

    fprintf(stderr, "-- expect a warning about a section with no ':' --\n");
    check(modeset_parse(&ms, "golz,icons;ops:clock"), "colon-less section dropped");
    check_order(&ms, want, 1, "no-colon");
}

static void test_duplicate_id_first_wins(void) {
    modeset_t ms;
    static const char *want[] = {"golz", "icons", "clock"};
    static const char *fun[] = {"golz", "icons"};
    static const char *ops[] = {"clock"};

    fprintf(stderr, "-- expect warnings about duplicate ids --\n");
    check(modeset_parse(&ms, "fun:golz,icons,golz;ops:clock,icons"),
          "duplicate-bearing spec used");
    check_order(&ms, want, 3, "dupes");
    check_section(&ms, 0, "fun", fun, 2, "dupes");
    check_section(&ms, 1, "ops", ops, 1, "dupes");
    check(strcmp(modeset_section_of(&ms, "icons"), "fun") == 0,
          "a duplicated id keeps its first section");
}

static void test_repeated_section_merges(void) {
    modeset_t ms;
    static const char *fun[] = {"golz", "palette"};

    check(modeset_parse(&ms, "fun:golz;ops:clock;fun:palette"),
          "repeated section name used");
    check(modeset_section_count(&ms) == 2, "repeat does not add a section");
    check_section(&ms, 0, "fun", fun, 2, "repeat");
}

/* Nothing usable at all must fall back to the full default, never to a blank
 * panel — the whole point of the "typo can't brick it" rule. */
static void test_empty_result_falls_back(void) {
    modeset_t ms;

    fprintf(stderr, "-- expect warnings about an unusable spec --\n");
    check(!modeset_parse(&ms, "fun:nosuchmode,alsobad"),
          "all-bad spec falls back to default");
    check_order(&ms, DEFAULT_ORDER, 9, "all-bad spec");

    check(!modeset_parse(&ms, ";;;"), "separator soup falls back to default");
    check_order(&ms, DEFAULT_ORDER, 9, "separator soup");

    check(!modeset_parse(&ms, "garbage"), "bare garbage falls back to default");
    check_order(&ms, DEFAULT_ORDER, 9, "bare garbage");
}

/* ---- bounds ----------------------------------------------------------- */

static void test_overflow_is_clamped(void) {
    modeset_t ms;
    char spec[1024];

    /* More sections than we hold: the extras are dropped, the rest survive. */
    fprintf(stderr, "-- expect warnings about section overflow --\n");
    check(modeset_parse(&ms, "fun:golz;ops:clock;fun:icons;ops:calc"),
          "repeats beyond two names still parse");
    check(modeset_section_count(&ms) <= MODESET_MAX_SECTIONS,
          "section count within bounds");

    /* A very long id must not overrun the id buffer. */
    snprintf(spec, sizeof(spec),
             "fun:%0*d;ops:clock", MODESET_ID_MAX + 40, 0);
    fprintf(stderr, "-- expect a warning about an over-long id --\n");
    check(modeset_parse(&ms, spec), "over-long id does not break the parse");
    check(modeset_count(&ms) == 1, "over-long id is skipped, clock survives");
    check(modeset_at(&ms, 0) && strcmp(modeset_at(&ms, 0), "clock") == 0,
          "clock is the survivor");
}

static void test_query_bounds(void) {
    modeset_t ms;
    modeset_default(&ms);

    check(modeset_at(&ms, -1) == NULL, "at(-1) is NULL");
    check(modeset_at(&ms, 9) == NULL, "at(count) is NULL");
    check(modeset_section_name(&ms, -1) == NULL, "section_name(-1) is NULL");
    check(modeset_section_name(&ms, 2) == NULL, "section_name(count) is NULL");
    check(modeset_section_size(&ms, 99) == 0, "section_size out of range is 0");
    check(modeset_section_at(&ms, 0, -1) == NULL, "section_at(-1) is NULL");
    check(modeset_section_at(&ms, 0, 99) == NULL, "section_at(oob) is NULL");
    check(modeset_section_of(&ms, "nope") == NULL, "section_of unknown is NULL");
    check(modeset_section_of(&ms, NULL) == NULL, "section_of NULL is NULL");
    check(!modeset_enabled(&ms, NULL), "enabled(NULL) is false");
}

int main(void) {
    test_default();
    test_unset_is_default();
    test_basic_parse();
    test_section_order_follows_spec();
    test_mode_order_follows_spec();
    test_whitespace_tolerated();
    test_empty_section_is_fine();
    test_unknown_id_skipped();
    test_unknown_section_skipped();
    test_missing_colon_skipped();
    test_duplicate_id_first_wins();
    test_repeated_section_merges();
    test_empty_result_falls_back();
    test_overflow_is_clamped();
    test_query_bounds();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("all modeset tests passed\n");
    return 0;
}
