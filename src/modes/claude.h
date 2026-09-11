/**
 * @file claude.h
 * Claude mode: fleet agent activity (sessions) and subscription usage-limit
 * gauges, read from the claude-feed Redis through libkdash (lib/kdashdata).
 *
 * The mode owns its own kdash handle, opened on the claude stem (kdashdata
 * CD-7) from the endpoint this panel is configured with. That is why the
 * constructor takes the endpoint: the handle exists exactly when the mode
 * does, so a panel that never registers `claude` never dials the feed without
 * main.c having to keep a second roster of which modes use which endpoint.
 */
#ifndef KDESKDASH_MODE_CLAUDE_H
#define KDESKDASH_MODE_CLAUDE_H

#include "mode.h"

/* `redis_auth` NULL means no AUTH. */
kd_mode_t *claude_mode_create(const char *id, const char *title,
                              const char *redis_host, int redis_port,
                              const char *redis_auth);

/* Close the feed handle. Safe on NULL, on a mode that never connected, and
 * called twice. */
void claude_mode_shutdown(kd_mode_t *self);

#endif /* KDESKDASH_MODE_CLAUDE_H */
