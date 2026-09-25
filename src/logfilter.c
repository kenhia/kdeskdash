/* logfilter.c — see logfilter.h for why this exists (WI #2657; kpidash WI #2646). */
#include "logfilter.h"

#include <stddef.h>
#include <string.h>

/* The literal LVGL emits from lv_draw_label.c:
 *     LV_LOG_WARN("lv_draw_letter: glyph dsc. not found for U+%" LV_PRIX32, letter);
 * Matched on the invariant middle of it. The `lv_draw_letter:` prefix is the
 * function name LVGL prepends and the `U+` is the format; matching on the
 * sentence between them survives either being reworded on an LVGL bump
 * without silently turning the filter off. */
static const char MARKER[] = "glyph dsc. not found for U+";

static uint32_t g_seen[LOGFILTER_MAX_CODEPOINTS];
static int g_seen_count = 0;

static int hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool logfilter_parse_codepoint(const char *msg, uint32_t *out) {
    if (!msg || !out)
        return false;

    const char *p = strstr(msg, MARKER);
    if (!p)
        return false;
    p += sizeof(MARKER) - 1;

    int d = hex_digit(*p);
    if (d < 0)
        return false;

    /* Six digits covers all of Unicode (U+10FFFF); a longer run is not a
     * codepoint and is not ours to suppress. */
    uint32_t cp = 0;
    int digits = 0;
    while ((d = hex_digit(*p)) >= 0) {
        if (++digits > 6)
            return false;
        cp = (cp << 4) | (uint32_t)d;
        p++;
    }

    *out = cp;
    return true;
}

logfilter_verdict_t logfilter_check(const char *msg) {
    uint32_t cp;
    if (!logfilter_parse_codepoint(msg, &cp))
        return LOGFILTER_EMIT;

    for (int i = 0; i < g_seen_count; i++) {
        if (g_seen[i] == cp)
            return LOGFILTER_SUPPRESS;
    }

    /* Table full. Suppress rather than emit: a panel that has already
     * reported 64 distinct missing characters has told the operator
     * everything this warning can tell them, and letting the 65th through on
     * every redraw is the flood the filter exists to stop. */
    if (g_seen_count >= LOGFILTER_MAX_CODEPOINTS)
        return LOGFILTER_SUPPRESS;

    g_seen[g_seen_count++] = cp;
    return LOGFILTER_EMIT_FIRST;
}

void logfilter_reset(void) {
    g_seen_count = 0;
}

int logfilter_reported_count(void) {
    return g_seen_count;
}
