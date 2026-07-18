/**
 * @file foreground.h
 * Remote-foreground ("R4gnd") mode: lists the live VS Code / Insiders windows on
 * the fleet (published by kvscf) in a 4×7 grid; tapping one publishes a focus
 * command that brings that window to the front on its host. The app glyph on the
 * left rail is rendered at runtime via TinyTTF. Reads + publishes on the kvscf
 * 6380 feed (see kvscf_redis.h), which main.c initialises.
 */
#ifndef KDESKDASH_MODE_FOREGROUND_H
#define KDESKDASH_MODE_FOREGROUND_H

#include "mode.h"

/* Create the foreground mode. `ttf_path` is the Symbols Nerd Font read at
 * runtime for the app-rail glyph (borrowed for the mode's lifetime). */
kd_mode_t *foreground_mode_create(const char *id, const char *title,
                                  const char *ttf_path);

#endif /* KDESKDASH_MODE_FOREGROUND_H */
