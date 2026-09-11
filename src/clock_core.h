/**
 * @file clock_core.h
 * Pure core behind the shared dual-clock widget (clock_widget.h): turning a
 * `time_t` into the strings a face shows, and choosing a type scale for the
 * space the widget was handed. No LVGL, host-testable.
 *
 * The widget is deliberately size-agnostic — it renders into a 576 px pane
 * beside the Launcher grid *and* (once WI #1136 lands) into a full 1920×440
 * screen. Everything that has to change between those two is the tier, which
 * is why the tier is arithmetic in here rather than a constant in there.
 */
#ifndef KDESKDASH_CLOCK_CORE_H
#define KDESKDASH_CLOCK_CORE_H

#include <time.h>

/* One clock face, pre-formatted. Sizes are the widest real value plus slack:
 * `zone` holds an abbreviation like "PDT" (glibc can return longer ones for
 * exotic zones, which simply clip — it is a caption). */
typedef struct {
    char hm[8];    /* "HH:MM", 24-hour */
    char sec[8];   /* ":SS" — a separate string so it can be set smaller */
    char date[24]; /* "Sat 18 Jul" */
    char zone[8];  /* "PDT" / "UTC" */
} kd_clock_face_t;

/* Format `t` in the process timezone (the caller pins TZ — see
 * kd_clock_widget_create). */
void kd_clock_local_face(time_t t, kd_clock_face_t *out);

/* Format `t` in UTC. `zone` is always "UTC". */
void kd_clock_utc_face(time_t t, kd_clock_face_t *out);

/* Format `t` in an arbitrary TZ-database zone ("Asia/Tokyo"), restoring the
 * process timezone afterwards — the local face depends on TZ staying pinned
 * where kd_clock_widget_create put it.
 *
 * An unknown zone name is not an error anywhere in the C library: glibc
 * silently falls back to UTC. So a mistyped zone shows UTC's time under
 * whatever label the config gave it, rather than failing. Zeroes `out` and
 * returns with empty strings when `tz` is NULL or empty. */
void kd_clock_zone_face(time_t t, const char *tz, kd_clock_face_t *out);

/* The almanac: the parts of a date that a wall clock does not show. Populated
 * from `t` in the process timezone. */
typedef struct {
    char long_date[40]; /* "Saturday 11 September 2026" */
    int  iso_week;      /* ISO-8601 week number, 1..53 */
    int  yday;          /* day of the year, 1..366 */
    int  ydays;         /* days in this year, 365 or 366 */
    /* How far through each unit, 0..99. Truncated rather than rounded, so a
     * unit reads 99 until it actually ends and 0 the moment it restarts —
     * a bar that sits at 100 for the last half hour of the day is lying. The
     * week runs Monday 00:00 to Sunday 24:00, matching the ISO week above. */
    int day_pct;
    int week_pct;
    int year_pct;
} kd_clock_almanac_t;

void kd_clock_almanac(time_t t, kd_clock_almanac_t *out);

/* A configured extra clock face: a TZ name and the label to show it under. */
#define KD_CLOCK_ZONES_MAX 4

typedef struct {
    char label[20];
    char tz[64];
} kd_clock_zone_t;

/* Parse a `KDESKDASH_CLOCK_ZONES` spec into `out`, returning how many entries
 * were written (0..max).
 *
 * Grammar: comma-separated entries, each `[<label>=]<tz>`, surrounding spaces
 * ignored. Without an explicit label the last path segment of the zone is used
 * with underscores turned into spaces, so `America/New_York` shows as
 * "New York".
 *
 * **Every malformed entry is skipped, never fatal** — an empty entry, a
 * missing zone, or one too long for the struct. A spec that yields nothing
 * simply means no extra faces, which is also the default. The same rule
 * `modeset` follows, for the same reason: a typo in an env var must not be
 * able to blank a panel. */
int kd_clock_parse_zones(const char *spec, kd_clock_zone_t *out, int max);

/* Type scale, chosen from the space available. The widget maps each tier onto
 * the built-in Montserrat sizes; the thresholds live here so they can be
 * asserted without a display. */
typedef enum {
    KD_CLOCK_TIER_XS = 0, /* cramped: hours:minutes only carries */
    KD_CLOCK_TIER_S,
    KD_CLOCK_TIER_M, /* the Launcher's 576×440 side pane */
    KD_CLOCK_TIER_L, /* a full-screen clock mode */
} kd_clock_tier_t;

/* Tier for a pane `w`×`h` pixels. Both dimensions bind: a tall narrow strip
 * cannot take the large face any more than a short wide one can. */
kd_clock_tier_t kd_clock_tier(int w, int h);

#endif /* KDESKDASH_CLOCK_CORE_H */
