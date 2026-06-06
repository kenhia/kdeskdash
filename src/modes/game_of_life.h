/**
 * @file game_of_life.h
 * Game of Life content mode: full-screen Conway's Life rendered on an LVGL
 * canvas, with toroidal wrap, randomized settings per activation, and an
 * optional fade trail.
 */
#ifndef KDESKDASH_MODE_GAME_OF_LIFE_H
#define KDESKDASH_MODE_GAME_OF_LIFE_H

#include "mode.h"

/* Create the Game of Life mode. `id`/`title` are borrowed string literals. */
kd_mode_t *game_of_life_mode_create(const char *id, const char *title);

#endif /* KDESKDASH_MODE_GAME_OF_LIFE_H */
