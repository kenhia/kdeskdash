/**
 * @file config.h
 * kdeskdash runtime configuration (environment overrides).
 */
#ifndef KDESKDASH_CONFIG_H
#define KDESKDASH_CONFIG_H

#include <stdbool.h>

typedef struct {
    const char *drm_dev;   /* KDESKDASH_DRM_DEV   — default /dev/dri/card1 (vc4 GPU) */
    const char *touch_dev; /* KDESKDASH_TOUCH_DEV — default /dev/input/event1 (ILITEK) */
    const char *redis_host; /* KDESKDASH_REDIS_HOST — default 127.0.0.1 */
    int         redis_port; /* KDESKDASH_REDIS_PORT — default 6379 */
    const char *redis_auth; /* REDISCLI_AUTH — NULL when unset (no AUTH) */
    bool        rotate_180; /* KDESKDASH_ROTATE_180 — flip the whole display 180° (case mounts the panel inverted) */
} kdeskdash_config_t;

/* Populate cfg from the environment, falling back to defaults. */
void config_load(kdeskdash_config_t *cfg);

#endif /* KDESKDASH_CONFIG_H */
