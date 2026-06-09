/**
 * @file golz.h
 * GoLZ content mode: full-screen two-faction Game of Life (living vs. zombies)
 * rendered on an LVGL canvas, with randomized per-round settings, a persistent
 * zombie-win counter, a tap control menu, and the zombie-win celebration.
 */
#ifndef KDESKDASH_MODE_GOLZ_H
#define KDESKDASH_MODE_GOLZ_H

#include "mode.h"

/* Create the GoLZ mode. `id`/`title` are borrowed string literals. */
kd_mode_t *golz_mode_create(const char *id, const char *title);

#endif /* KDESKDASH_MODE_GOLZ_H */
