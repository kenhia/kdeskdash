/* logfilter.h — per-codepoint dedupe for LVGL's missing-glyph warning (WI #2657).
 *
 * Ported unchanged from kpidash (WI #2646), where it was measured first.
 * LVGL logs `lv_draw_letter: glyph dsc. not found for U+<hex>` from
 * lv_draw_label.c on EVERY redraw of EVERY character the font does not
 * carry. A single unlucky character in one Service Card therefore writes to
 * the journal about once every two seconds, forever: WI #363 measured it for
 * `°`, and WI #2646 measured 3,844 lines in two hours for `·`.
 *
 * The warning is worth having exactly once per codepoint — it names the
 * character a publisher used and the font lacks. Every repeat after that is
 * noise that buries everything else the panel logs.
 *
 * This is the pure half: it decides, it does not print. main.c registers the
 * printing half with lv_log_register_print_cb(), which is why lv_conf.h sets
 * LV_LOG_PRINTF to 0 — LVGL's own printf path is additive with the callback,
 * so leaving it on would emit every line twice and dedupe neither.
 */
#ifndef KD_LOGFILTER_H
#define KD_LOGFILTER_H

#include <stdbool.h>
#include <stdint.h>

/* How many distinct missing codepoints are tracked before the table is full.
 * Far more than a healthy panel ever sees: the table exists to bound memory,
 * not to be reached. */
#define LOGFILTER_MAX_CODEPOINTS 64

typedef enum {
    /* Not a missing-glyph warning, or the first sighting of one — print it. */
    LOGFILTER_EMIT = 0,
    /* First sighting of this codepoint — print it, and say that the repeats
     * are being dropped so a reader of the journal is not misled into
     * thinking the character was drawn only once. */
    LOGFILTER_EMIT_FIRST,
    /* This codepoint has already been reported — drop it. */
    LOGFILTER_SUPPRESS,
} logfilter_verdict_t;

/* Classify one formatted LVGL log line.
 *
 * `msg` is the whole line LVGL hands the print callback, prefix and
 * file/line suffix included; the codepoint is parsed out of it wherever it
 * appears. A NULL or non-matching line is always LOGFILTER_EMIT, so nothing
 * but the missing-glyph warning can ever be suppressed.
 *
 * Not thread-safe, and does not need to be: LVGL logs from the render
 * thread, which is the only thread that draws. */
logfilter_verdict_t logfilter_check(const char *msg);

/* Parse the codepoint out of a missing-glyph warning.
 * Returns true and writes *out on a match; false otherwise. Exposed for the
 * tests, and because it is the one part with an off-by-one to get wrong. */
bool logfilter_parse_codepoint(const char *msg, uint32_t *out);

/* Forget every codepoint seen so far. Tests only — the dashboard never
 * wants a warning it has already reported to come back. */
void logfilter_reset(void);

/* How many distinct codepoints have been reported so far. Tests only. */
int logfilter_reported_count(void);

#endif /* KD_LOGFILTER_H */
