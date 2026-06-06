/**
 * @file menu.h
 * Menu launcher mode: the swipe-down target. Shows one tile per registered
 * content mode; tapping a tile opens that mode.
 */
#ifndef KDESKDASH_MODE_MENU_H
#define KDESKDASH_MODE_MENU_H

#include "mode.h"

/* Create the Menu launcher mode. `id`/`title` are borrowed string literals. */
kd_mode_t *menu_mode_create(const char *id, const char *title);

#endif /* KDESKDASH_MODE_MENU_H */
