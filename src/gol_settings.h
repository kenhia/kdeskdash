/**
 * @file gol_settings.h
 * Pure validation for one untrusted Game of Life settings field, extracted from
 * the hiredis-coupled redis.c so the per-field clamps (which have drifted once
 * before) can be compiled and unit-tested on the host with no Redis dependency.
 *
 * The bounds are hard safety limits, intentionally wider than random_settings()
 * in game_of_life.c so a remote client can experiment — except the cell_size
 * floor (>= 2), which preserves the bounded worst-case grid / per-frame work.
 */
#ifndef KDESKDASH_GOL_SETTINGS_H
#define KDESKDASH_GOL_SETTINGS_H

#include <stdbool.h>

#include "gol.h"

/* Apply one untrusted "field"="val" pair onto cfg. Recognized numeric fields
 * are parsed strictly (whole token must be a base-10 integer / float, ASCII
 * whitespace tolerated) and applied only when in range; anything else — an
 * unknown field, a non-numeric or overflowing value, or an out-of-range value —
 * is ignored, leaving cfg untouched so the caller's defaults survive.
 *
 * Returns true iff a recognized field was applied. NULL args return false. */
bool gol_settings_apply_field(gol_settings_t *cfg, const char *field,
                              const char *val);

#endif /* KDESKDASH_GOL_SETTINGS_H */
