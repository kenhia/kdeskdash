/**
 * @file test_quickswitch.c
 * Host-only unit tests for the double-tap quick-switch core (no LVGL).
 */
#include <stdio.h>
#include <string.h>

#include "quickswitch.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void target_is(const quickswitch_t *q, const char *want, const char *what) {
    const char *got = quickswitch_target(q);
    if (!want) {
        if (got) {
            fprintf(stderr, "FAIL: %s: got \"%s\", want NULL\n", what, got);
            failures++;
        }
        return;
    }
    if (!got || strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL: %s: got \"%s\", want \"%s\"\n", what,
                got ? got : "(null)", want);
        failures++;
    }
}

int main(void) {
    /* --- tap timing --- */
    {
        quickswitch_t q;
        quickswitch_init(&q, NULL);
        check(!quickswitch_tap(&q, 1000), "first tap is not a double-tap");
        check(quickswitch_tap(&q, 1000 + QUICKSWITCH_WINDOW_MS), "tap at the window edge fires");

        quickswitch_init(&q, NULL);
        check(!quickswitch_tap(&q, 1000), "first tap");
        check(!quickswitch_tap(&q, 1000 + QUICKSWITCH_WINDOW_MS + 1), "one ms late does not fire");
        /* ...and that late tap became the new first tap. */
        check(quickswitch_tap(&q, 1000 + QUICKSWITCH_WINDOW_MS + 2), "late tap restarts the pair");

        /* Three fast taps are one switch, not two — otherwise a jittery panel
         * toggles twice and lands back where it started. */
        quickswitch_init(&q, NULL);
        check(!quickswitch_tap(&q, 0), "tap 1 of 3");
        check(quickswitch_tap(&q, 50), "tap 2 of 3 fires");
        check(!quickswitch_tap(&q, 100), "tap 3 of 3 does not fire again");
        check(quickswitch_tap(&q, 150), "tap 4 fires");

        /* now_ms == 0 is a real tick value, not "unset". */
        quickswitch_init(&q, NULL);
        check(!quickswitch_tap(&q, 0), "first tap at tick 0");
        check(quickswitch_tap(&q, 10), "second tap after a tick-0 first");

        /* The LVGL tick wraps every ~49 days; a double-tap across the wrap
         * must still register. */
        quickswitch_init(&q, NULL);
        check(!quickswitch_tap(&q, 0xFFFFFF00u), "tap just before wrap");
        check(quickswitch_tap(&q, 0x00000010u), "tap after wrap still pairs");

        check(!quickswitch_tap(NULL, 0), "NULL state is safe");
    }

    /* --- most-recent fallback (no configuration at all) --- */
    {
        quickswitch_t q;
        quickswitch_init(&q, NULL);
        target_is(&q, NULL, "nothing active yet");

        quickswitch_note_active(&q, "claude");
        target_is(&q, NULL, "only one mode seen -> nothing to switch to");

        quickswitch_note_active(&q, "foreground");
        target_is(&q, "claude", "bounce back to the previous mode");

        /* Re-activating the current mode must not make it its own partner. */
        quickswitch_note_active(&q, "foreground");
        target_is(&q, "claude", "repeat activation changes nothing");

        quickswitch_note_active(&q, "claude");
        target_is(&q, "foreground", "and back again");

        /* A third mode moves the pair along — this is MRU, not a fixed pair. */
        quickswitch_note_active(&q, "calc");
        target_is(&q, "claude", "newest previous wins");

        quickswitch_note_active(&q, NULL);
        target_is(&q, "claude", "NULL id ignored");
        quickswitch_note_active(&q, "");
        target_is(&q, "claude", "empty id ignored");
    }

    /* --- configured pairs win over the fallback --- */
    {
        quickswitch_t q;
        quickswitch_init(&q, "claude:foreground,clock:calc");
        check(q.pair_count == 2, "two pairs parsed");

        quickswitch_note_active(&q, "claude");
        target_is(&q, "foreground", "pair resolves with no history at all");

        quickswitch_note_active(&q, "foreground");
        target_is(&q, "claude", "pair is symmetric");

        quickswitch_note_active(&q, "clock");
        target_is(&q, "calc", "second pair, and it beats the MRU (foreground)");

        /* A mode in no pair still falls back to most-recent. */
        quickswitch_note_active(&q, "palette");
        target_is(&q, "clock", "unpaired mode falls back to previous");
    }

    /* --- spec degradation: warn and skip, never lose the whole feature --- */
    {
        quickswitch_t q;
        quickswitch_init(&q, "");
        check(q.pair_count == 0, "empty spec -> no pairs");
        quickswitch_init(&q, NULL);
        check(q.pair_count == 0, "NULL spec -> no pairs");

        quickswitch_init(&q, "claude:foreground, clock:calc");
        check(q.pair_count == 2, "spaces around an entry are tolerated");
        quickswitch_note_active(&q, "clock");
        target_is(&q, "calc", "space-tolerant entry actually works");

        /* One bad entry must not cost the good one. */
        quickswitch_init(&q, "garbage,claude:foreground");
        check(q.pair_count == 1, "malformed entry skipped, good one kept");
        quickswitch_note_active(&q, "claude");
        target_is(&q, "foreground", "survivor works");

        quickswitch_init(&q, "a:,:b,a:b:c,,x:y");
        check(q.pair_count == 1, "empty sides, triples and blanks all skipped");
        quickswitch_note_active(&q, "x");
        target_is(&q, "y", "only the well-formed pair survived");

        /* Over-long ids are rejected rather than silently truncated into a
         * partner nothing matches. */
        char big[QUICKSWITCH_ID_MAX * 3];
        snprintf(big, sizeof(big), "%0*d:ok", QUICKSWITCH_ID_MAX + 4, 0);
        quickswitch_init(&q, big);
        check(q.pair_count == 0, "over-long id rejected, not truncated");

        /* The table has a bound and overflowing it is not fatal. */
        quickswitch_init(&q, "a:b,c:d,e:f,g:h,i:j,k:l,m:n,o:p,q:r,s:t");
        check(q.pair_count == QUICKSWITCH_MAX_PAIRS, "pair table capped, not overrun");

        quickswitch_init(NULL, "a:b"); /* must not crash */
        check(quickswitch_target(NULL) == NULL, "NULL state target is NULL");
    }

    /* --- the off switch: a default you cannot opt out of is not a default --- */
    {
        quickswitch_t q;
        quickswitch_init(&q, "none");
        check(q.disabled, "\"none\" disables");
        check(q.pair_count == 0, "\"none\" configures no pairs");
        /* Inert end to end: the tap never completes and there is never a target,
         * even once two modes have been seen (which is exactly when the
         * previous-mode fallback would otherwise fire). */
        quickswitch_note_active(&q, "claude");
        quickswitch_note_active(&q, "foreground");
        target_is(&q, NULL, "disabled: no fallback target");
        check(!quickswitch_tap(&q, 1000), "disabled: first tap inert");
        check(!quickswitch_tap(&q, 1050), "disabled: second tap inert too");

        /* Spellings. "none" is canonical; "off" is accepted because a reader who
         * knows env_flag's vocabulary will try it, and the failure mode
         * otherwise is a silent "malformed pair" warning plus a still-live
         * gesture. */
        quickswitch_init(&q, "NONE");  check(q.disabled, "case-insensitive NONE");
        quickswitch_init(&q, "None");  check(q.disabled, "case-insensitive None");
        quickswitch_init(&q, "off");   check(q.disabled, "\"off\" also disables");
        quickswitch_init(&q, "OFF");   check(q.disabled, "case-insensitive OFF");
        quickswitch_init(&q, "  none  "); check(q.disabled, "surrounding space tolerated");

        /* Not the off switch: unset/empty is the working default, and a mode
         * genuinely called "none" on one side of a pair is still a pair. */
        quickswitch_init(&q, NULL);
        check(!q.disabled, "unset is NOT disabled");
        quickswitch_init(&q, "");
        check(!q.disabled, "empty is NOT disabled");
        quickswitch_init(&q, "none:calc");
        check(!q.disabled, "\"none:calc\" is a pair, not the off switch");
        check(q.pair_count == 1, "and it parsed");
        quickswitch_note_active(&q, "none");
        target_is(&q, "calc", "a mode named \"none\" still pairs");
        /* A longer word merely starting with "none" is not the off switch. */
        quickswitch_init(&q, "nonesuch:calc");
        check(!q.disabled, "\"nonesuch:calc\" is not the off switch");
    }

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_quickswitch: all passed\n");
    return 0;
}
