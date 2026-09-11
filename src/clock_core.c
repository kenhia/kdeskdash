/**
 * @file clock_core.c
 * Pure clock formatting + type-scale selection — see clock_core.h.
 */
#include "clock_core.h"

#include <stdbool.h>
#include <stdlib.h>
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

void kd_clock_zone_face(time_t t, const char *tz, kd_clock_face_t *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!tz || tz[0] == '\0')
        return;

    /* getenv returns a pointer *into* the environment, and setenv below may
     * reallocate it — so the old value has to be copied out before, not
     * dereferenced after. An absent TZ is restored as absent rather than as
     * the zone we borrowed, which is what the unset case actually means. */
    const char *saved = getenv("TZ");
    char        saved_copy[64];
    bool        had_tz = saved != NULL;
    if (had_tz)
        snprintf(saved_copy, sizeof(saved_copy), "%s", saved);

    setenv("TZ", tz, 1);
    tzset();
    struct tm tm;
    localtime_r(&t, &tm);
    fill_face(&tm, NULL, out);

    if (had_tz)
        setenv("TZ", saved_copy, 1);
    else
        unsetenv("TZ");
    tzset();
}

static bool is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

void kd_clock_almanac(time_t t, kd_clock_almanac_t *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));

    struct tm tm;
    localtime_r(&t, &tm);

    /* "%A %-d %B %Y" — the wall clock already shows the short form; what a
     * full screen has room for is the one that spells everything out. */
    strftime(out->long_date, sizeof(out->long_date), "%A %-d %B %Y", &tm);

    out->yday = tm.tm_yday + 1; /* tm_yday is 0-based */
    out->ydays = is_leap(tm.tm_year + 1900) ? 366 : 365;

    /* ISO-8601 week, which is not tm_yday/7: its weeks start on Monday and
     * week 1 is the one holding the first Thursday, so 31 December can be
     * week 1 of the *following* year. strftime knows the rule; we do not
     * need to. */
    char wk[8];
    strftime(wk, sizeof(wk), "%V", &tm);
    out->iso_week = atoi(wk);

    /* Progress through the day, the week and the year. All three are derived
     * from the same local wall clock the rest of the almanac uses, so they
     * agree with the date above them across a DST boundary — where a naive
     * "seconds since midnight / 86400" from a UTC timestamp would not. */
    long secs_today = tm.tm_hour * 3600L + tm.tm_min * 60L + tm.tm_sec;
    /* ISO weekday: Monday 0 .. Sunday 6 (tm_wday is Sunday 0). */
    long iso_wday = (tm.tm_wday + 6) % 7;

    out->day_pct = (int)(secs_today * 100 / 86400L);
    out->week_pct = (int)((iso_wday * 86400L + secs_today) * 100 / (7 * 86400L));
    out->year_pct = (int)(((long)(out->yday - 1) * 86400L + secs_today) * 100 /
                          ((long)out->ydays * 86400L));
}

/* Copy src[0..len) into dst, trimming spaces off both ends. Returns false when
 * nothing is left, or when what is left cannot fit — a *truncated* zone name
 * is the dangerous case, because "Asia/Tok" is not an error to the C library,
 * it is UTC under a plausible-looking name. */
static bool trim_copy(char *dst, size_t dstsz, const char *src, size_t len) {
    while (len > 0 && *src == ' ') {
        src++;
        len--;
    }
    while (len > 0 && src[len - 1] == ' ')
        len--;
    if (len == 0 || len >= dstsz)
        return false;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return true;
}

/* "America/New_York" -> "New York". A label is only ever displayed, so unlike
 * the zone name it may be truncated rather than dropped. */
static void label_from_tz(char *dst, size_t dstsz, const char *tz) {
    const char *seg = strrchr(tz, '/');
    seg = seg ? seg + 1 : tz;
    snprintf(dst, dstsz, "%s", seg);
    for (char *p = dst; *p; p++)
        if (*p == '_')
            *p = ' ';
}

int kd_clock_parse_zones(const char *spec, kd_clock_zone_t *out, int max) {
    if (!spec || !out || max <= 0)
        return 0;
    if (max > KD_CLOCK_ZONES_MAX)
        max = KD_CLOCK_ZONES_MAX;

    int n = 0;
    const char *p = spec;
    while (*p && n < max) {
        const char *comma = strchr(p, ',');
        size_t      len = comma ? (size_t)(comma - p) : strlen(p);

        const char *eq = memchr(p, '=', len);
        kd_clock_zone_t z;
        bool ok;
        if (eq) {
            /* Both halves must be there: "=zone" and "label=" are typos, and
             * guessing which half was meant is worse than skipping the row. */
            ok = trim_copy(z.label, sizeof(z.label), p, (size_t)(eq - p)) &&
                 trim_copy(z.tz, sizeof(z.tz), eq + 1,
                           len - (size_t)(eq - p) - 1);
        } else {
            ok = trim_copy(z.tz, sizeof(z.tz), p, len);
            if (ok)
                label_from_tz(z.label, sizeof(z.label), z.tz);
        }
        if (ok)
            out[n++] = z;

        if (!comma)
            break;
        p = comma + 1;
    }
    return n;
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
