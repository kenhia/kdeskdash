/**
 * @file clock.h
 * Clock content mode: large centered local time, UTC on the left, and a
 * wall-clock stopwatch (Start/Stop + Reset) on the right.
 */
#ifndef KDESKDASH_MODE_CLOCK_H
#define KDESKDASH_MODE_CLOCK_H

#include "mode.h"

/* Create the Clock mode. `id`/`title` are borrowed string literals. */
kd_mode_t *clock_mode_create(const char *id, const char *title);

#endif /* KDESKDASH_MODE_CLOCK_H */
