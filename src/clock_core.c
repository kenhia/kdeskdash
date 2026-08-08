/**
 * @file clock_core.c
 * Pure clock formatting + type-scale selection — see clock_core.h.
 */
#include "clock_core.h"

#include <stdio.h>
#include <string.h>

static void fill_face(const struct tm *tm, const char *zone,
                      kd_clock_face_t *out) {
    memset(out, 0, sizeof(*out));
    strftime(out->hm, sizeof(out->hm), "%H:%M", tm);
    strftime(out->sec, sizeof(out->sec), ":%S", tm);
    /* "%a %-d %b" — day-first, no leading zero. Fixed C locale on the panel, so
     * this is stable rather than surprising. */
    strftime(out->date, sizeof(out->date), "%a %-d %b", tm);
    if (zone)
        snprintf(out->zone, sizeof(out->zone), "%s", zone);
    else
        strftime(out->zone, sizeof(out->zone), "%Z", tm);
}

void kd_clock_local_face(time_t t, kd_clock_face_t *out) {
    if (!out)
        return;
    struct tm tm;
    localtime_r(&t, &tm);
    fill_face(&tm, NULL, out); /* %Z — PDT/PST, whichever is in force */
}

void kd_clock_utc_face(time_t t, kd_clock_face_t *out) {
    if (!out)
        return;
    struct tm tm;
    gmtime_r(&t, &tm);
    /* gmtime_r's %Z is "GMT" on glibc; the panel says UTC, so name it. */
    fill_face(&tm, "UTC", out);
}

kd_clock_tier_t kd_clock_tier(int w, int h) {
    /* Thresholds are the space the stacked layout needs at each scale: the big
     * "HH:MM" plus its seconds, a date line, and the second face beneath.
     *
     * Calibrated against the panel, not guessed. The first cut asked for 900 px
     * of width before granting the large scale, on the theory that "large"
     * meant "full screen" — which left the Launcher's 576 px pane rendering a
     * small clock in a mostly empty box. What the large scale actually needs is
     * room for "16:25" at 48 px plus its seconds, about 250 px. Width stopped
     * being the interesting constraint; height is. */
    if (w >= 520 && h >= 380)
        return KD_CLOCK_TIER_L;
    if (w >= 420 && h >= 300)
        return KD_CLOCK_TIER_M;
    if (w >= 300 && h >= 200)
        return KD_CLOCK_TIER_S;
    return KD_CLOCK_TIER_XS;
}
