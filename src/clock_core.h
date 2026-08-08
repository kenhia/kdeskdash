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
