/**
 * @file config.c
 * kdeskdash runtime configuration (environment overrides).
 */
#include "config.h"

#include <stdlib.h>
#include <string.h>
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

    /* kvscf endpoint (foreground mode). On rpidash2 the kvscf keys live on the
     * same instance as the claude feed, so unset means "reuse the claude-feed
     * values" and that device's env needs no change. A second panel reads the
     * same fleet claude feed but drives a *different* kvscf, which is what this
     * split exists for. Host and port fall back independently — set only the
     * host and you inherit the claude port. Auth does NOT; see below. */
    cfg->kvscf_redis_host =
        env_or("KDESKDASH_KVSCF_REDIS_HOST", cfg->claude_redis_host);
    int kport = atoi(env_or("KDESKDASH_KVSCF_REDIS_PORT", "0"));
    cfg->kvscf_redis_port =
        (kport > 0 && kport <= 65535) ? kport : cfg->claude_redis_port;
    /* The auth fallback follows the ENDPOINT, not the variable. Inheriting the
     * claude password is only ever right when kvscf is the *same instance* —
     * which is the entire reason the fallback exists. Once the two endpoints
     * differ, inheriting is actively wrong, and wrong in the way that reads
     * worst: a Redis with no password configured answers AUTH with an ERROR,
     * not a shrug, so the handle never connects and the panel reports "kvscf
     * feed unavailable" as though the endpoint were down.
     *
     * Sprint 031 hit exactly that. Repointing the claude feed to central gave
     * that handle a password for the first time, and rpidash2's kvscf — the
     * same loopback instance as always, still passwordless — inherited it and
     * stopped connecting. Pinning host and port was not enough, and no value
     * of KDESKDASH_KVSCF_REDISCLI_AUTH could say "this one takes none": empty
     * means unset, which means inherit. */
    const char *kauth = getenv("KDESKDASH_KVSCF_REDISCLI_AUTH");
    bool same_instance =
        cfg->kvscf_redis_port == cfg->claude_redis_port &&
        strcmp(cfg->kvscf_redis_host, cfg->claude_redis_host) == 0;
    cfg->kvscf_redis_auth = (kauth && kauth[0] != '\0')
                                ? kauth
                                : (same_instance ? cfg->claude_redis_auth : NULL);

    /* Icons mode: the runtime Symbols Nerd Font (deployed as a file) and the
     * favourites list it curates. Both default to system paths the deploy sets
     * up; overridable for dev runs pointing at the in-repo TTF. */
    cfg->icons_ttf_path = env_or("KDESKDASH_ICONS_TTF",
                                 "/usr/local/share/kdeskdash/SymbolsNerdFont-Regular.ttf");
    cfg->icons_favorites_path =
        env_or("KDESKDASH_ICONS_FAVORITES", "/var/lib/kdeskdash/icon-favorites.txt");

    /* Foreground mode: shared secret authenticating window-focus commands to
     * kvscf on cleo. Empty when unset (focusing disabled); kvscf_redis trims any
     * trailing CR/LF before use, since it must byte-match the cleo-side secret. */
    cfg->kvscf_token = env_or("KVSCF_TOKEN", "");

    /* Per-device mode set. NULL (unset or empty) means the full built-in set —
     * the modeset core owns the grammar and every degradation path. */
    cfg->modes_spec = env_or("KDESKDASH_MODES", NULL);

    /* Double-tap quick switch — three reachable states, because a default
     * nobody can opt out of is not a default:
     *
     *   unset      the partner is the previously active mode (the working
     *              default: the Claude/Remote case needs no configuration)
     *   "none"     the gesture is inert ("off" is accepted too)
     *   "a:b,c:d"  pinned pairs, which win over the fallback
     *
     * Passed through verbatim: quickswitch.c owns the grammar and every
     * degradation path, the same split as modeset.c and KDESKDASH_MODES. */
    cfg->quick_pairs = env_or("KDESKDASH_QUICK_PAIRS", NULL);

    /* kpidash service card (write-only). The board lives on the same Redis the
     * telemetry feed reads, so unset means "reuse the telemetry values" and no
     * device env needs a new line. Host and port fall back independently.
     *
     * Auth follows the ENDPOINT, not the variable — the same rule, and the same
     * reason, as the kvscf/claude split above: inheriting a password is only
     * ever right when it is the same instance, and a passwordless Redis answers
     * AUTH with an ERROR rather than a shrug, so an inherited password reads on
     * the panel as an endpoint that is simply down. */
    cfg->card_redis_host =
        env_or("KDESKDASH_CARD_REDIS_HOST", cfg->telemetry_redis_host);
    int dport = atoi(env_or("KDESKDASH_CARD_REDIS_PORT", "0"));
    cfg->card_redis_port =
        (dport > 0 && dport <= 65535) ? dport : cfg->telemetry_redis_port;
    const char *dauth = getenv("KDESKDASH_CARD_REDISCLI_AUTH");
    bool card_same_instance =
        cfg->card_redis_port == cfg->telemetry_redis_port &&
        strcmp(cfg->card_redis_host, cfg->telemetry_redis_host) == 0;
    cfg->card_redis_auth =
        (dauth && dauth[0] != '\0')
            ? dauth
            : (card_same_instance ? cfg->telemetry_redis_auth : NULL);

    /* The card's name segment. Identity on the board is (name, host), so two
     * panels on two hosts need nothing here; two instances on ONE host would
     * clobber each other's key and must be given distinct names. */
    cfg->card_name = env_or("KDESKDASH_CARD_NAME", "deskdash");
}
