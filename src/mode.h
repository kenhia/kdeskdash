/**
 * @file mode.h
 * A dashboard "mode" — a self-contained full-screen view (Game of Life, Clock,
 * Menu, ...) with an activate/deactivate/tick lifecycle.
 *
 * The shell owns navigation and lifecycle; each mode owns its LVGL screen and
 * its private state. Adding a new mode is one registration call (see shell.h).
 */
#ifndef KDESKDASH_MODE_H
#define KDESKDASH_MODE_H

#include "lvgl.h"

typedef struct kd_mode kd_mode_t;

struct kd_mode {
    const char *id;    /* stable id, also used as the Redis active-mode value */
    const char *title; /* human-readable label shown in the menu */
    lv_obj_t   *screen; /* owning LVGL screen; may be created lazily in activate */

    /* Called when the mode becomes visible. Must ensure `screen` is non-NULL. */
    void (*activate)(kd_mode_t *self);
    /* Called when the mode is hidden. A deactivated mode must do no ongoing work. */
    void (*deactivate)(kd_mode_t *self);
    /* Called every main-loop iteration while the mode is active. May be NULL. */
    void (*tick)(kd_mode_t *self);

    void *state; /* mode-private data */
};

#endif /* KDESKDASH_MODE_H */
