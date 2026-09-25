/* test_logfilter.c — per-codepoint dedupe of LVGL's missing-glyph warning
 * (WI #2657, ported from kpidash WI #2646). The behaviour under test is "loud once, silent after", and the
 * thing that must never regress is the other half: a line that is not a
 * missing-glyph warning is never suppressed, however much it repeats. */
#include "logfilter.h"

#include <stdio.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #expr);                      \
            failed++;                                                                              \
        } else {                                                                                   \
            passed++;                                                                              \
        }                                                                                          \
    } while (0)

/* The shape lv_log_add() hands the print callback, with LV_LOG_USE_TIMESTAMP
 * and LV_LOG_USE_FILE_LINE both at their defaults. Copied from a real rpi53
 * journal line rather than invented, because the parse is the point. */
static const char *WARN_B7 = "[Warn]\t(12.345, +1)\t lv_draw_letter: glyph dsc. not found for "
                             "U+B7 \t(lv_draw_label.c:393)\n";
static const char *WARN_2014 = "[Warn]\t(12.350, +5)\t lv_draw_letter: glyph dsc. not found for "
                               "U+2014 \t(lv_draw_label.c:393)\n";

int main(void) {
    /* --- parse ---------------------------------------------------------- */
    uint32_t cp = 0;
    CHECK(logfilter_parse_codepoint(WARN_B7, &cp));
    CHECK(cp == 0xB7);
    cp = 0;
    CHECK(logfilter_parse_codepoint(WARN_2014, &cp));
    CHECK(cp == 0x2014);

    /* Bare message, no prefix or suffix — the parse must not depend on them. */
    cp = 0;
    CHECK(logfilter_parse_codepoint("lv_draw_letter: glyph dsc. not found for U+1F600", &cp));
    CHECK(cp == 0x1F600);

    /* Lower-case hex: LV_PRIX32 is upper-case today, but the parse costs
     * nothing to make insensitive and a format change should not resurrect
     * the flood. */
    cp = 0;
    CHECK(logfilter_parse_codepoint("glyph dsc. not found for U+2e3b", &cp));
    CHECK(cp == 0x2E3B);

    /* Non-matches. */
    CHECK(!logfilter_parse_codepoint(NULL, &cp));
    CHECK(!logfilter_parse_codepoint("", &cp));
    CHECK(!logfilter_parse_codepoint("[Warn] something else entirely\n", &cp));
    /* The marker with no hex digits after it is not a warning we can dedupe. */
    CHECK(!logfilter_parse_codepoint("glyph dsc. not found for U+", &cp));
    CHECK(!logfilter_parse_codepoint("glyph dsc. not found for U+zz", &cp));

    /* --- verdicts ------------------------------------------------------- */
    logfilter_reset();
    CHECK(logfilter_reported_count() == 0);

    /* First sighting is loud; every repeat after it is silent. */
    CHECK(logfilter_check(WARN_B7) == LOGFILTER_EMIT_FIRST);
    CHECK(logfilter_reported_count() == 1);
    for (int i = 0; i < 2000; i++) {
        CHECK(logfilter_check(WARN_B7) == LOGFILTER_SUPPRESS);
    }
    CHECK(logfilter_reported_count() == 1);

    /* A different codepoint is a different fact and gets its own first. */
    CHECK(logfilter_check(WARN_2014) == LOGFILTER_EMIT_FIRST);
    CHECK(logfilter_check(WARN_2014) == LOGFILTER_SUPPRESS);
    CHECK(logfilter_check(WARN_B7) == LOGFILTER_SUPPRESS);
    CHECK(logfilter_reported_count() == 2);

    /* --- nothing else is ever suppressed -------------------------------- */
    for (int i = 0; i < 100; i++) {
        CHECK(logfilter_check("[Warn]\t(1.000, +1)\t lv_obj_del: deleting an object\n") ==
              LOGFILTER_EMIT);
        CHECK(logfilter_check("[Error]\t(1.000, +1)\t out of memory\n") == LOGFILTER_EMIT);
    }
    CHECK(logfilter_check(NULL) == LOGFILTER_EMIT);
    CHECK(logfilter_check("") == LOGFILTER_EMIT);
    /* Count unchanged: non-warnings must not consume table slots. */
    CHECK(logfilter_reported_count() == 2);

    /* --- a full table stays bounded and stays quiet --------------------- */
    logfilter_reset();
    char buf[128];
    for (int i = 0; i < LOGFILTER_MAX_CODEPOINTS; i++) {
        snprintf(buf, sizeof(buf), "glyph dsc. not found for U+%X", 0x3000 + i);
        CHECK(logfilter_check(buf) == LOGFILTER_EMIT_FIRST);
    }
    CHECK(logfilter_reported_count() == LOGFILTER_MAX_CODEPOINTS);

    /* Past the cap: a codepoint that does not fit is suppressed rather than
     * emitted, because emitting it is the flood this exists to stop. The
     * table never grows past its bound. */
    for (int i = 0; i < 500; i++) {
        snprintf(buf, sizeof(buf), "glyph dsc. not found for U+%X", 0x4000 + i);
        CHECK(logfilter_check(buf) == LOGFILTER_SUPPRESS);
    }
    CHECK(logfilter_reported_count() == LOGFILTER_MAX_CODEPOINTS);

    /* Codepoints already in the full table still resolve correctly. */
    CHECK(logfilter_check("glyph dsc. not found for U+3000") == LOGFILTER_SUPPRESS);

    /* reset() really empties it. */
    logfilter_reset();
    CHECK(logfilter_reported_count() == 0);
    CHECK(logfilter_check(WARN_B7) == LOGFILTER_EMIT_FIRST);

    printf("test_logfilter: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
