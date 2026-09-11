/**
 * @file quickswitch.h
 * Pure core for the double-tap quick switch: tap-timing, the configured mode
 * pairs, and the most-recent fallback. No LVGL, no Redis.
 *
 * The need (WI 1032): some modes get switched between constantly — Claude and
 * Remote is the standing example — and doing it by swipe means knowing the
 * cycle order and how far round it is. A double-tap on the background toggles
 * instead.
 *
 * Two ways to say what the partner is, and the first that matches wins:
 *
 *  1. A **configured pair** (`KDESKDASH_QUICK_PAIRS="claude:foreground"`) —
 *     deterministic, and what the work item asked for.
 *  2. The **previously active mode**, when no pair covers the current one. This
 *     costs no configuration and is what makes the feature useful on a panel
 *     nobody has set up: swipe to anything, then double-tap to bounce between
 *     the two. Configure a pair only to pin the partner regardless of history.
 *
 * LVGL 9.2.2 has no double-click event and no click-streak API, so the timing
 * lives here where it can be tested without a panel.
 */
#ifndef KDESKDASH_QUICKSWITCH_H
#define KDESKDASH_QUICKSWITCH_H

#include <stdbool.h>
#include <stdint.h>

#define QUICKSWITCH_MAX_PAIRS 8
#define QUICKSWITCH_ID_MAX    32

/* Max gap between the two taps. Long enough for a deliberate double-tap on a
 * capacitive panel, short enough that two unrelated taps are not one. */
#define QUICKSWITCH_WINDOW_MS 400u

typedef struct {
    char     a[QUICKSWITCH_MAX_PAIRS][QUICKSWITCH_ID_MAX];
    char     b[QUICKSWITCH_MAX_PAIRS][QUICKSWITCH_ID_MAX];
    int      pair_count;
    char     current[QUICKSWITCH_ID_MAX];
    char     previous[QUICKSWITCH_ID_MAX];
    uint32_t last_tap_ms;
    bool     tap_pending;
} quickswitch_t;

/* Reset and parse `spec` — `"<id>:<id>[,<id>:<id>]..."`. NULL/empty is valid
 * and simply means no configured pairs (the fallback still works). Every
 * malformed entry is warned about and skipped rather than failing the parse: a
 * typo in one device's env must not cost the whole feature. */
void quickswitch_init(quickswitch_t *q, const char *spec);

/* Record the mode that just became active. Repeats of the current id are
 * ignored, so the "previous" slot always names a genuinely different mode. */
void quickswitch_note_active(quickswitch_t *q, const char *id);

/* Feed one tap. Returns true when this tap *completes* a double-tap (the
 * previous tap was within QUICKSWITCH_WINDOW_MS), and consumes it, so three
 * fast taps fire once rather than twice. */
bool quickswitch_tap(quickswitch_t *q, uint32_t now_ms);

/* The mode id to switch to, or NULL when there is nothing to switch to (no
 * pair covers the current mode and nothing else has been active yet). A
 * configured pair always wins over the most-recent fallback. */
const char *quickswitch_target(const quickswitch_t *q);

#endif /* KDESKDASH_QUICKSWITCH_H */
