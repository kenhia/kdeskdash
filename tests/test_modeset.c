/**
 * @file test_modeset.c
 * Host-only unit tests for the per-device mode-set core: the KDESKDASH_MODES
 * grammar, the built-in default, dedupe, unknown-id and malformed-section
 * handling, ordering, the section query API, and the configurable sections of
 * WI 668 (names, header colours, capacity by count).
 *
 * The contract these lock down is "a typo must never blank the panel": every
 * malformed input path has to degrade to something usable, never to nothing.
 */
#include <stdio.h>
#include <string.h>

#include "modeset.h"
#include "palette.h"

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

static const char *DEFAULT_ORDER[] = {
    "game_of_life", "golz",  "icons", "palette",  "claude",
    "foreground",   "launcher", "clock", "dev",   "calc"};
static const char *DEFAULT_FUN[] = {"game_of_life", "golz", "icons", "palette"};
static const char *DEFAULT_OPS[] = {"claude", "foreground", "launcher",
                                    "clock",  "dev",        "calc"};
#define DEFAULT_N ((int)(sizeof(DEFAULT_ORDER) / sizeof(DEFAULT_ORDER[0])))

static void test_default(void) {
    modeset_t ms;
    modeset_default(&ms);

    check_order(&ms, DEFAULT_ORDER, DEFAULT_N, "default");
    check(modeset_section_count(&ms) == 2, "default has two sections");
    check_section(&ms, 0, "fun", DEFAULT_FUN, 4, "default");
    check_section(&ms, 1, "ops", DEFAULT_OPS, 6, "default");

    /* Every id in the roster is enabled by default, and nothing else is. */
    check(modeset_enabled(&ms, "palette"), "default enables palette");
    check(modeset_enabled(&ms, "dev"), "default enables dev");
    check(!modeset_enabled(&ms, "menu"), "menu is not a content mode");
    check(!modeset_enabled(&ms, "nope"), "unknown id not enabled");

    /* The roster doubles as the known-id list. */
    check(modeset_id_known("game_of_life"), "game_of_life is a known id");
    check(modeset_id_known("calc"), "calc is a known id");
    check(modeset_id_known("launcher"), "launcher is a known id (sprint 025)");
    check(!modeset_id_known(""), "empty string is not a known id");
    check(!modeset_id_known(NULL), "NULL is not a known id");
}

/* An unset or empty var must land exactly on the default. */
static void test_unset_is_default(void) {
    modeset_t ms;

    check(!modeset_parse(&ms, NULL), "NULL spec reports 'used the default'");
    check_order(&ms, DEFAULT_ORDER, DEFAULT_N, "NULL spec");

    check(!modeset_parse(&ms, ""), "empty spec reports 'used the default'");
    check_order(&ms, DEFAULT_ORDER, DEFAULT_N, "empty spec");

    check(!modeset_parse(&ms, "   \t "), "blank spec reports 'used the default'");
    check_order(&ms, DEFAULT_ORDER, DEFAULT_N, "blank spec");
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

/* Since WI 668 a section name is a display label, not a lookup: anything
 * non-empty is legal. What is still malformed is a clause with no name. */
static void test_nameless_section_skipped(void) {
    modeset_t ms;
    static const char *want[] = {"clock"};

    fprintf(stderr, "-- expect a warning about a section with no name --\n");
    check(modeset_parse(&ms, ":golz;ops:clock"),
          "spec with one nameless section is still used");
    check_order(&ms, want, 1, "nameless-section");
    check(modeset_section_count(&ms) == 1, "nameless section is dropped whole");
    check(!modeset_enabled(&ms, "golz"), "modes under it are dropped");

    fprintf(stderr, "-- expect a warning about a section with no name --\n");
    check(modeset_parse(&ms, "#EDGE_TEAL:golz;ops:clock"),
          "colour with no name is nameless too");
    check(!modeset_enabled(&ms, "golz"), "colour-only clause is dropped");
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
    check_order(&ms, DEFAULT_ORDER, DEFAULT_N, "all-bad spec");

    check(!modeset_parse(&ms, ";;;"), "separator soup falls back to default");
    check_order(&ms, DEFAULT_ORDER, DEFAULT_N, "separator soup");

    check(!modeset_parse(&ms, "garbage"), "bare garbage falls back to default");
    check_order(&ms, DEFAULT_ORDER, DEFAULT_N, "bare garbage");
}

/* ---- configurable sections (WI 668) ---------------------------------- */

/* The configs every panel runs today must parse exactly as they did: same
 * names (the menu title-cases them), the default coral header, 9 per side. */
static void test_legacy_spec_unchanged(void) {
    modeset_t ms;

    check(modeset_parse(&ms, "fun:game_of_life,golz,icons,palette;"
                             "ops:claude,launcher,foreground,clock,dev,calc"),
          "rpidash2's spec used");
    check(modeset_section_count(&ms) == 2, "legacy: two sections");
    check(strcmp(modeset_section_name(&ms, 0), "fun") == 0, "legacy: fun");
    check(strcmp(modeset_section_name(&ms, 1), "ops") == 0, "legacy: ops");
    check(modeset_section_color(&ms, 0) == KD_PAL_CLAUDE_CORAL,
          "legacy: fun header is coral");
    check(modeset_section_color(&ms, 1) == KD_PAL_CLAUDE_CORAL,
          "legacy: ops header is coral");
    check(modeset_section_capacity(&ms) == 9, "legacy: 9 tiles per section");

    modeset_default(&ms);
    check(modeset_section_color(&ms, 0) == KD_PAL_CLAUDE_CORAL,
          "default: coral header");
    check(modeset_section_capacity(&ms) == 9, "default: 9 per section");
}

static void test_arbitrary_names(void) {
    modeset_t ms;
    static const char *play[] = {"golz", "icons"};
    static const char *work[] = {"clock"};

    check(modeset_parse(&ms, "Play:golz,icons; Work Stuff :clock"),
          "arbitrary names used");
    check_section(&ms, 0, "Play", play, 2, "names");
    check_section(&ms, 1, "Work Stuff", work, 1, "names (trimmed, inner space kept)");
    check(strcmp(modeset_section_of(&ms, "clock"), "Work Stuff") == 0,
          "section_of returns the authored name");
}

static void test_header_colours(void) {
    modeset_t ms;

    check(modeset_parse(&ms, "Fun#EDGE_TEAL:golz;Ops # code_blue :clock;Etc:calc"),
          "coloured spec used");
    check(strcmp(modeset_section_name(&ms, 0), "Fun") == 0, "colour not in name");
    check(strcmp(modeset_section_name(&ms, 1), "Ops") == 0,
          "spaced colour not in name");
    check(modeset_section_color(&ms, 0) == KD_PAL_EDGE_TEAL, "EDGE_TEAL resolved");
    check(modeset_section_color(&ms, 1) == KD_PAL_CODE_BLUE,
          "colour lookup is case-insensitive and trimmed");
    check(modeset_section_color(&ms, 2) == KD_PAL_CLAUDE_CORAL,
          "no colour means the default coral");
    check(modeset_section_color(&ms, 3) == -1, "colour out of range is -1");
    check(modeset_section_color(&ms, -1) == -1, "colour(-1) is -1");
}

/* An unknown colour is cosmetic: warn and fall back, never drop the modes. */
static void test_unknown_colour_falls_back(void) {
    modeset_t ms;

    fprintf(stderr, "-- expect warnings about colours 'NOPE' and '' --\n");
    check(modeset_parse(&ms, "Fun#NOPE:golz;Ops#:clock"), "bad colours still used");
    check(modeset_count(&ms) == 2, "bad colour keeps its modes");
    check(strcmp(modeset_section_name(&ms, 0), "Fun") == 0, "name survives");
    check(modeset_section_color(&ms, 0) == KD_PAL_CLAUDE_CORAL,
          "unknown colour falls back to coral");
    check(modeset_section_color(&ms, 1) == KD_PAL_CLAUDE_CORAL,
          "empty colour falls back to coral");
}

/* A repeated name merges (as before); the first clause's colour stands. */
static void test_repeated_section_keeps_first_colour(void) {
    modeset_t ms;

    fprintf(stderr, "-- expect a warning about a conflicting colour --\n");
    check(modeset_parse(&ms, "Fun#EDGE_TEAL:golz;Fun#CODE_BLUE:icons"),
          "repeat with colours used");
    check(modeset_section_count(&ms) == 1, "repeat merges");
    check(modeset_section_size(&ms, 0) == 2, "merged modes");
    check(modeset_section_color(&ms, 0) == KD_PAL_EDGE_TEAL, "first colour wins");
}

/* The menu's capacity per section is derived from the count: one section is a
 * single 18-tile grid, two are 9 each, three are 6 each. */
static void test_capacity_follows_count(void) {
    modeset_t ms;

    check(modeset_parse(&ms, "All:golz,icons,palette,clock,dev,calc"), "one used");
    check(modeset_section_count(&ms) == 1, "one section");
    check(modeset_section_capacity(&ms) == 18, "one section holds 18");

    check(modeset_parse(&ms, "A:golz;B:clock"), "two used");
    check(modeset_section_capacity(&ms) == 9, "two sections hold 9 each");

    check(modeset_parse(&ms, "A:golz;B:clock;C#STAR_GOLD:calc"), "three used");
    check(modeset_section_count(&ms) == 3, "three sections");
    check(modeset_section_capacity(&ms) == 6, "three sections hold 6 each");
    check(modeset_section_color(&ms, 2) == KD_PAL_STAR_GOLD, "third has its colour");

    fprintf(stderr, "-- expect a warning about a fourth section --\n");
    check(modeset_parse(&ms, "A:golz;B:clock;C:calc;D:dev"), "four parses");
    check(modeset_section_count(&ms) == 3, "fourth section dropped");
    check(!modeset_enabled(&ms, "dev"), "fourth section's modes dropped");
}

/* A name too long for the buffer is skipped like an over-long id — never
 * truncated into a different label. */
static void test_overlong_name_skipped(void) {
    modeset_t ms;
    char spec[256];

    snprintf(spec, sizeof(spec), "%0*d:golz;ops:clock", MODESET_NAME_MAX + 4, 0);
    fprintf(stderr, "-- expect a warning about an over-long section name --\n");
    check(modeset_parse(&ms, spec), "over-long name does not break the parse");
    check(modeset_section_count(&ms) == 1, "over-long section dropped");
    check(!modeset_enabled(&ms, "golz"), "its modes dropped");
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
    check(modeset_at(&ms, DEFAULT_N) == NULL, "at(count) is NULL");
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
    test_nameless_section_skipped();
    test_legacy_spec_unchanged();
    test_arbitrary_names();
    test_header_colours();
    test_unknown_colour_falls_back();
    test_repeated_section_keeps_first_colour();
    test_capacity_follows_count();
    test_overlong_name_skipped();
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
