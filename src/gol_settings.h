/**
 * @file gol_settings.h
 * Pure validation for one untrusted settings field — the Conway struct and the
 * GoLZ one — extracted from the hiredis-coupled redis.c so the per-field clamps
 * (which have drifted once before) can be compiled and unit-tested on the host
 * with no Redis dependency.
 *
 * The bounds are hard safety limits, intentionally wider than random_settings()
 * in game_of_life.c so a remote client can experiment — except the cell_size
 * floor (>= 2), which preserves the bounded worst-case grid / per-frame work.
 *
 * Both appliers take a NAME and a TEXT value, because that is what arrives:
 * a Redis HASH field until sprint 039, and a `settings` entry of
 * `kdash:panelmode:<host>` since. The contract deliberately keeps setting
 * values as text (CD-22) so there is no number formatting for two consumers to
 * disagree about, which is why this boundary did not have to change shape when
 * the transport did.
 */
#ifndef KDESKDASH_GOL_SETTINGS_H
#define KDESKDASH_GOL_SETTINGS_H

#include <stdbool.h>

#include "gol.h"
#include "golz.h"

/* Apply one untrusted "field"="val" pair onto cfg. Recognized numeric fields
 * are parsed strictly (whole token must be a base-10 integer / float, ASCII
 * whitespace tolerated) and applied only when in range; anything else — an
 * unknown field, a non-numeric or overflowing value, or an out-of-range value —
 * is ignored, leaving cfg untouched so the caller's defaults survive.
 *
 * Returns true iff a recognized field was applied. NULL args return false. */
bool gol_settings_apply_field(gol_settings_t *cfg, const char *field,
                              const char *val);

/* The same, for the GoLZ-only settings. Same contract in every respect: an
 * unknown field, a non-numeric or overflowing value, or an out-of-range value
 * leaves cfg untouched and returns false.
 *
 * It lived in redis.c until sprint 039 and parsed with atoi(), which coerces
 * "abc" and "3.9" to 0 — i.e. an out-of-band value silently became
 * initial_count=0 or max_generations=0-then-rejected. Moving it here put it on
 * the same strict parse its Conway sibling has had since the boundary was
 * extracted. */
bool golz_settings_apply_field(golz_settings_t *cfg, const char *field,
                               const char *val);

#endif /* KDESKDASH_GOL_SETTINGS_H */
