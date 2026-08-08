/**
 * @file launcher.h
 * Launcher mode — the touch surface that retires the Stream Deck.
 */
#ifndef KDESKDASH_MODES_LAUNCHER_H
#define KDESKDASH_MODES_LAUNCHER_H

#include "mode.h"

/* Build the Launcher mode. Reads the kvscf launcher feed and publishes button
 * presses on the shared kvscf handle (kvscf_redis.h), so a panel registering
 * this mode must have kvscf_redis_init'd — see main.c. */
kd_mode_t *launcher_mode_create(const char *id, const char *title);

#endif /* KDESKDASH_MODES_LAUNCHER_H */
