/**
 * @file placeholder.h
 * Temporary placeholder modes used to verify shell navigation in Unit 1.
 * Replaced by the real Game of Life, Clock, and Menu modes in later units.
 */
#ifndef KDESKDASH_PLACEHOLDER_H
#define KDESKDASH_PLACEHOLDER_H

#include "mode.h"

#include <stdint.h>

/* Create a placeholder mode with a coloured screen, a big title, a tap button
 * (to confirm taps still fire CLICKED and do not navigate), and a hint label.
 * The returned mode and its screen are heap-allocated and never freed (they
 * live for the program's lifetime). */
kd_mode_t *placeholder_mode_create(const char *id, const char *title, uint32_t bg);

#endif /* KDESKDASH_PLACEHOLDER_H */
