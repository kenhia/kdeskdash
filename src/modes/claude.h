/**
 * @file claude.h
 * Claude mode: fleet agent activity (sessions, recent completions) and
 * subscription usage-limit gauges, fed by the claude-feed Redis.
 */
#ifndef KDESKDASH_MODE_CLAUDE_H
#define KDESKDASH_MODE_CLAUDE_H

#include "mode.h"

kd_mode_t *claude_mode_create(const char *id, const char *title);

#endif /* KDESKDASH_MODE_CLAUDE_H */
