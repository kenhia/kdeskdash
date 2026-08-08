/**
 * @file clock_widget.h
 * The shared dual-clock (local + UTC) widget.
 *
 * Built for the Launcher's right-hand pane, where it replaces the two Stream
 * Deck keys that were showing exactly these two times — but built as a widget
 * rather than as launcher-private drawing because the `clock` mode is being
 * rebuilt on it (WI #1136) and Ken asked for one implementation, not two.
 *
 * So: it takes a **parent container** and sizes itself to whatever that
 * container turns out to be, picking its type scale from the measured space
 * (clock_core.h). It never creates a screen and never assumes 1920×440.
 *
 * `create` pins the process timezone (see CLOCK_LOCAL_TZ) so the "local" face
 * is the desk's local time regardless of the device's system TZ — the same
 * thing `modes/clock.c` does today, and the reason that call is not idempotent
 * across differing zones.
 */
#ifndef KDESKDASH_CLOCK_WIDGET_H
#define KDESKDASH_CLOCK_WIDGET_H

#include "lvgl.h"

typedef struct kd_clock_widget kd_clock_widget_t;

/* Build the clock inside `parent`, filling it. The parent's layout is measured
 * here, so give it its final size first. Returns NULL only on allocation
 * failure. The widget owns nothing outside `parent`'s subtree; deleting the
 * parent (or its screen) deletes the objects, so call destroy to free the
 * handle itself. */
kd_clock_widget_t *kd_clock_widget_create(lv_obj_t *parent);

/* Refresh from the wall clock. Cheap and safe to call every tick: the labels
 * are only touched when their text actually changes, so a 100 Hz main loop
 * repaints the seconds once a second and nothing else. */
void kd_clock_widget_tick(kd_clock_widget_t *w);

/* Free the handle. Does not delete the LVGL objects — their parent owns them. */
void kd_clock_widget_destroy(kd_clock_widget_t *w);

#endif /* KDESKDASH_CLOCK_WIDGET_H */
