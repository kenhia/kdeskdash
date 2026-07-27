/**
 * @file menu.h
 * Menu launcher mode: the swipe-down target. Shows one tile per registered
 * content mode, grouped into the sections the modeset declares; tapping a tile
 * opens that mode.
 */
#ifndef KDESKDASH_MODE_MENU_H
#define KDESKDASH_MODE_MENU_H

#include "mode.h"
#include "modeset.h"

/* Create the Menu launcher mode. `id`/`title` are borrowed string literals.
 * `modes` supplies the section names, membership and order; it is borrowed and
 * must outlive the mode (main.c keeps it static). NULL falls back to the
 * built-in default set. */
kd_mode_t *menu_mode_create(const char *id, const char *title,
                            const modeset_t *modes);

#endif /* KDESKDASH_MODE_MENU_H */
