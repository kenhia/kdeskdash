/**
 * @file config.c
 * kdeskdash runtime configuration (environment overrides).
 */
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "panel_cmd.h"
#include "panel_state.h"

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

    /* The board's OWN Redis on 6379 — loopback-only and passwordless on both
     * panels. Since sprint 039 the panel reads it EXACTLY ONCE, on a first run
     * with no state file, to copy the durable values it used to keep there
     * into /var/lib/kdeskdash/state. Nothing else dials it, and after the
     * k-homelab cleanup slice it will not be there to dial.
     *
     * It reads KDESKDASH_CONTROL_REDISCLI_AUTH
     * and deliberately NOT bare REDISCLI_AUTH, which since sprint 037 is the
     * fleet-wide name for the CENTRAL rpi53 password that k-homelab renders
     * into /etc/khomelab/secrets.env on every host.
     *
     * Reading the bare name here would be the sprint-031 failure with a new
     * face: the unit gained the fleet file, this handle would suddenly send
     * AUTH to a Redis that has no password configured, and Redis answers that
     * with an ERROR rather than a shrug, so the connection never opens.
     * Measured on both boards before the change:
     * `REDISCLI_AUTH=x redis-cli -p 6379 ping` ->
     * "ERR AUTH <password> called without any password configured".
     *
     * What that costs has SHRUNK, and the name is still the right one. Until
     * sprint 039 it was remote mode control, last-mode persistence, GoL
     * injection and the screenshot trigger, reported on the panel as nothing
     * at all; now it is one migration that finds no old scores to copy. The
     * failure is quieter, not gone, and unit-lint still pins the name. */
    cfg->redis_host = env_or("KDESKDASH_REDIS_HOST", "127.0.0.1");
    int port = atoi(env_or("KDESKDASH_REDIS_PORT", "6379"));
    cfg->redis_port = (port > 0 && port <= 65535) ? port : 6379;
    const char *auth = getenv("KDESKDASH_CONTROL_REDISCLI_AUTH");
    cfg->redis_auth = (auth && auth[0] != '\0') ? auth : NULL;

    /* Where the durable state lives. The unit's StateDirectory=kdeskdash makes
     * this the one writable path under ProtectSystem=strict. */
    cfg->state_path = env_or("KDESKDASH_STATE_FILE", PANEL_STATE_DEFAULT_PATH);

    cfg->rotate_180 = env_flag("KDESKDASH_ROTATE_180");

    /* Telemetry source: kpidash publishes host metrics to a (typically remote)
     * Redis. Its own endpoint and handle, independent of every other feed. */
    cfg->telemetry_redis_host = env_or("KDESKDASH_TELEMETRY_REDIS_HOST", "rpi53");
    int tport = atoi(env_or("KDESKDASH_TELEMETRY_REDIS_PORT", "6379"));
    cfg->telemetry_redis_port = (tport > 0 && tport <= 65535) ? tport : 6379;
    /* REDISCLI_AUTH is the fleet's one name for the central rpi53 password
     * (k-homelab sprint 059). Telemetry and the claude feed are two connections
     * to that one endpoint, so they are one secret and read one name — which is
     * what retired this panel's two private copies of it. */
    const char *tauth = getenv("REDISCLI_AUTH");
    cfg->telemetry_redis_auth = (tauth && tauth[0] != '\0') ? tauth : NULL;

    /* Claude feed: agent activity + usage limits, published by the fleet to a
     * second Redis instance on this same Pi — a localhost read by default,
     * deliberately independent of the (remote, flakier) telemetry endpoint. */
    cfg->claude_redis_host = env_or("KDESKDASH_CLAUDE_REDIS_HOST", "127.0.0.1");
    int cport = atoi(env_or("KDESKDASH_CLAUDE_REDIS_PORT", "6380"));
    cfg->claude_redis_port = (cport > 0 && cport <= 65535) ? cport : 6380;
    /* Same endpoint as telemetry, same secret, same name — a separate handle
     * for failure isolation, not a separate credential. */
    const char *cauth = getenv("REDISCLI_AUTH");
    cfg->claude_redis_auth = (cauth && cauth[0] != '\0') ? cauth : NULL;

    /* kvscf endpoint (foreground + launcher modes). Its OWN endpoint, with its
     * own defaults — the claude-feed inheritance that used to live here is
     * gone (korg WI 2305).
     *
     * It existed because rpidash2 once kept both feeds on one loopback
     * instance, so "unset" honestly meant "the same place". Sprint 031 moved
     * the claude feed to central and the sameness ended; from then on an unset
     * kvscf endpoint followed the claude feed to rpi53 and found no kvscf
     * there, which is a default that cannot be right on any board. Both panels
     * had already pinned 127.0.0.1:6380 by hand, so those pins ARE the honest
     * default and are now compiled in.
     * See docs/solutions/best-practices/a-fallback-outlives-the-sameness-that-justified-it.md */
    cfg->kvscf_redis_host = env_or("KDESKDASH_KVSCF_REDIS_HOST", "127.0.0.1");
    int kport = atoi(env_or("KDESKDASH_KVSCF_REDIS_PORT", "6380"));
    cfg->kvscf_redis_port = (kport > 0 && kport <= 65535) ? kport : 6380;

    /* Which fleet key holds THIS board's kvscf password — named by the device's
     * own config, never guessed.
     *
     * k-homelab's WI 2399 chose "one key name per SECRET", and bin/check-secrets
     * refuses one name meaning two store entries, so the two desks' passwords
     * have two published names. Until now this was an ordered lookup over both
     * (KVSCF_REDISCLI_AUTH then CLAUDE_REDISCLI_AUTH), which worked only
     * because the endpoint was always the board's own instance.
     *
     * The fold breaks that, and measurably: on rpidash2 BOTH names are present
     * in the panel's environment and they are different secrets —
     * CLAUDE_REDISCLI_AUTH is redis-claude's (:6380) and must stay until the
     * k-homelab cleanup retires that server, while REDISCLI_AUTH is central's.
     * An ordered lookup would send the :6380 password to rpi53, and AUTH with
     * the wrong password is an ERROR, not a shrug — the handle never opens and
     * the panel reports "kvscf feed unavailable" as though central were down.
     * That is sprint 031's failure with a new face, which is precisely why the
     * fleet rule (kxeneon WI 2734) is to never list two names for one endpoint.
     *
     * So the board says which name is its own. The legacy ordered lookup stays
     * as the unset path so a device whose env file predates this still works —
     * it is correct for exactly the case it was written for, a panel reading
     * its own board's instance. */
    const char *kauth_key = getenv("KDESKDASH_KVSCF_REDIS_AUTH_KEY");
    const char *kauth;
    if (kauth_key && kauth_key[0] != '\0') {
        kauth = getenv(kauth_key);
    } else {
        kauth = getenv("KVSCF_REDISCLI_AUTH");   /* redis-kvscf-auth-rpidash3  */
        if (!kauth || kauth[0] == '\0')
            kauth = getenv("CLAUDE_REDISCLI_AUTH"); /* redis-claude-auth-rpidash2 */
    }
    cfg->kvscf_redis_auth = (kauth && kauth[0] != '\0') ? kauth : NULL;

    /* The one workstation this panel is paired with. Empty/unset keeps the
     * pre-fold wildcard, which is still right for a panel reading a private
     * instance only its own pair writes to (rpidash3). On central the host
     * segment is the only scoping left, so rpidash2 names `cleo` — kdashdata
     * CD-8's obligation on this slice. kvscf_feed.c owns the semantics. */
    cfg->kvscf_pair_host = env_or("KDESKDASH_KVSCF_PAIR_HOST", NULL);

    /* Icons mode: the runtime Symbols Nerd Font (deployed as a file) and the
     * favourites list it curates. Both default to system paths the deploy sets
     * up; overridable for dev runs pointing at the in-repo TTF. */
    cfg->icons_ttf_path = env_or("KDESKDASH_ICONS_TTF",
                                 "/usr/local/share/kdeskdash/SymbolsNerdFont-Regular.ttf");
    cfg->icons_favorites_path =
        env_or("KDESKDASH_ICONS_FAVORITES", "/var/lib/kdeskdash/icon-favorites.txt");

    /* The pairing token authenticating focus/launch/press commands to this
     * panel's workstation. Empty when unset (the modes render read-only and
     * refuse to send); kvscf_redis trims any trailing CR/LF before use, since
     * it must byte-match the workstation-side secret.
     *
     * Since k-homelab sprint 069 both desks' tokens are age-store entries
     * rendered into the fleet's per-host /etc/khomelab/secrets.env — under a
     * name PER PAIR, not one shared name (KCTRLDECK_TOKEN_CLEO_PAIR on
     * rpidash2, KCTRLDECK_TOKEN_KWORK_PAIR on rpidash3), because the two desks
     * hold different values and bin/check-secrets refuses one name meaning two
     * secrets. As with the password, the board names its own key rather than
     * this reading a list that spans both desks: a fallback across two names
     * whose values must never be swapped is the failure that shows up as taps
     * quietly doing nothing.
     *
     * KVSCF_TOKEN is the deprecated last rung — the hand-installed
     * /etc/kdeskdash/secrets.env each board still carries. It is deleted per
     * board once the named read is proven there, and this rung goes with the
     * last one. */
    const char *ktok_key = getenv("KDESKDASH_KVSCF_TOKEN_KEY");
    const char *ktok = NULL;
    if (ktok_key && ktok_key[0] != '\0')
        ktok = getenv(ktok_key);
    if (!ktok || ktok[0] == '\0')
        ktok = getenv("KVSCF_TOKEN");
    cfg->kvscf_token = (ktok && ktok[0] != '\0') ? ktok : "";

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

    /* --- commands from central (sprint 039) --------------------------------
     *
     * `kdash:panelmode:<host>` / `kdash:panelshot:<host>` live on the CENTRAL
     * Redis, which is where the telemetry feed already points — so unset means
     * "reuse the telemetry values" and neither device's env file needs a new
     * endpoint line. Host and port fall back independently; auth follows the
     * ENDPOINT, not the variable, which is the sprint-031 rule the kvscf and
     * service-card blocks above both carry and for the same reason. */
    cfg->cmd_redis_host =
        env_or("KDESKDASH_CMD_REDIS_HOST", cfg->telemetry_redis_host);
    int mport = atoi(env_or("KDESKDASH_CMD_REDIS_PORT", "0"));
    cfg->cmd_redis_port =
        (mport > 0 && mport <= 65535) ? mport : cfg->telemetry_redis_port;
    const char *mauth = getenv("KDESKDASH_CMD_REDISCLI_AUTH");
    bool cmd_same_instance =
        cfg->cmd_redis_port == cfg->telemetry_redis_port &&
        strcmp(cfg->cmd_redis_host, cfg->telemetry_redis_host) == 0;
    cfg->cmd_redis_auth =
        (mauth && mauth[0] != '\0')
            ? mauth
            : (cmd_same_instance ? cfg->telemetry_redis_auth : NULL);

    /* The `{host}` segment this panel answers to. Derived, not configured, in
     * the normal case — one fewer line per device to get wrong, and a wrong
     * one here means the panel silently answers another panel's commands or
     * nobody's.
     *
     * Derived by LOWERCASING the first label, because gethostname() on
     * rpidash2 returns `rpiDash2` (korg WI 2277 — it is why that board's
     * service-card key is mixed-case) while the fleet inventory, k-homelab and
     * kdash-pub all say `rpidash2`. The panel must answer to the name people
     * and tools actually write.
     *
     * "" when the hostname is unreadable or not a legal token: panel_feed
     * treats that as "disable the command feed" rather than guessing. */
    static char panel_host[PANEL_CMD_HOST_MAX];
    const char *forced = env_or("KDESKDASH_PANEL_HOST", NULL);
    if (forced) {
        if (!panel_cmd_host(forced, panel_host, sizeof(panel_host)))
            panel_host[0] = '\0';
    } else {
        char raw[256];
        if (gethostname(raw, sizeof(raw)) != 0)
            raw[0] = '\0';
        raw[sizeof(raw) - 1] = '\0';
        if (!panel_cmd_host(raw, panel_host, sizeof(panel_host)))
            panel_host[0] = '\0';
    }
    cfg->panel_host = panel_host;
}
