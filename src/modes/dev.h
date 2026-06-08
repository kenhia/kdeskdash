/**
 * @file dev.h
 * Dev content mode: a mirrored four-chart host-telemetry view —
 * `[CPU/RAM][GPU/VRAM]  selector  [GPU/VRAM][CPU/RAM]` — driven by the
 * telemetry Redis client. Two hosts (left/right) each feed a CPU/RAM and a
 * GPU/VRAM chart, with GPU/VRAM inner on both sides.
 */
#ifndef KDESKDASH_MODE_DEV_H
#define KDESKDASH_MODE_DEV_H

#include "mode.h"

/* Create the Dev mode. `id`/`title` are borrowed string literals. */
kd_mode_t *dev_mode_create(const char *id, const char *title);

#endif /* KDESKDASH_MODE_DEV_H */
