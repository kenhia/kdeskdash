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

    cfg->redis_host = env_or("KDESKDASH_REDIS_HOST", "127.0.0.1");
    int port = atoi(env_or("KDESKDASH_REDIS_PORT", "6379"));
    cfg->redis_port = (port > 0 && port <= 65535) ? port : 6379;
    const char *auth = getenv("REDISCLI_AUTH");
    cfg->redis_auth = (auth && auth[0] != '\0') ? auth : NULL;
}
