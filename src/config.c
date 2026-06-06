/**
 * @file config.c
 * kdeskdash runtime configuration (environment overrides).
 */
#include "config.h"

#include <stdlib.h>

#define DEFAULT_DRM_DEV   "/dev/dri/card1"
#define DEFAULT_TOUCH_DEV "/dev/input/event1"

static const char *env_or(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return (v && v[0] != '\0') ? v : fallback;
}

void config_load(kdeskdash_config_t *cfg) {
    cfg->drm_dev = env_or("KDESKDASH_DRM_DEV", DEFAULT_DRM_DEV);
    cfg->touch_dev = env_or("KDESKDASH_TOUCH_DEV", DEFAULT_TOUCH_DEV);
}
