/**
 * @file config.c
 * kdeskdash runtime configuration (environment overrides).
 */
#include "config.h"

#include <stdlib.h>
#include <strings.h>

#define DEFAULT_DRM_DEV   "/dev/dri/card1"
/* Stable by-id symlink survives replug/reboot; event node numbers do not. */
#define DEFAULT_TOUCH_DEV "/dev/input/by-id/usb-ILITEK_ILITEK-TOUCH-event-if00"

static const char *env_or(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return (v && v[0] != '\0') ? v : fallback;
}

/* Truthy env flag: "1", "true", "yes", "on" (case-insensitive). Anything else
 * (unset, empty, "0", "off", or garbage) is false. */
static bool env_flag(const char *name) {
    const char *v = getenv(name);
    if (!v || v[0] == '\0')
        return false;
    return strcasecmp(v, "1") == 0 || strcasecmp(v, "true") == 0 ||
           strcasecmp(v, "yes") == 0 || strcasecmp(v, "on") == 0;
}

void config_load(kdeskdash_config_t *cfg) {
    cfg->drm_dev = env_or("KDESKDASH_DRM_DEV", DEFAULT_DRM_DEV);
    cfg->touch_dev = env_or("KDESKDASH_TOUCH_DEV", DEFAULT_TOUCH_DEV);

    cfg->redis_host = env_or("KDESKDASH_REDIS_HOST", "127.0.0.1");
    int port = atoi(env_or("KDESKDASH_REDIS_PORT", "6379"));
    cfg->redis_port = (port > 0 && port <= 65535) ? port : 6379;
    const char *auth = getenv("REDISCLI_AUTH");
    cfg->redis_auth = (auth && auth[0] != '\0') ? auth : NULL;

    cfg->rotate_180 = env_flag("KDESKDASH_ROTATE_180");

    /* Telemetry source: kpidash publishes host metrics to a (typically remote)
     * Redis. Separate endpoint + auth from the local control Redis. */
    cfg->telemetry_redis_host = env_or("KDESKDASH_TELEMETRY_REDIS_HOST", "rpi53");
    int tport = atoi(env_or("KDESKDASH_TELEMETRY_REDIS_PORT", "6379"));
    cfg->telemetry_redis_port = (tport > 0 && tport <= 65535) ? tport : 6379;
    const char *tauth = getenv("KDESKDASH_TELEMETRY_REDISCLI_AUTH");
    cfg->telemetry_redis_auth = (tauth && tauth[0] != '\0') ? tauth : NULL;

    /* Claude feed: agent activity + usage limits, published by the fleet to a
     * second Redis instance on this same Pi — a localhost read by default,
     * deliberately independent of the (remote, flakier) telemetry endpoint. */
    cfg->claude_redis_host = env_or("KDESKDASH_CLAUDE_REDIS_HOST", "127.0.0.1");
    int cport = atoi(env_or("KDESKDASH_CLAUDE_REDIS_PORT", "6380"));
    cfg->claude_redis_port = (cport > 0 && cport <= 65535) ? cport : 6380;
    const char *cauth = getenv("KDESKDASH_CLAUDE_REDISCLI_AUTH");
    cfg->claude_redis_auth = (cauth && cauth[0] != '\0') ? cauth : NULL;

    /* Icons mode: the runtime Symbols Nerd Font (deployed as a file) and the
     * favourites list it curates. Both default to system paths the deploy sets
     * up; overridable for dev runs pointing at the in-repo TTF. */
    cfg->icons_ttf_path = env_or("KDESKDASH_ICONS_TTF",
                                 "/usr/local/share/kdeskdash/SymbolsNerdFont-Regular.ttf");
    cfg->icons_favorites_path =
        env_or("KDESKDASH_ICONS_FAVORITES", "/var/lib/kdeskdash/icon-favorites.txt");
}
