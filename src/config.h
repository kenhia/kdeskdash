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
    const char *redis_host; /* KDESKDASH_REDIS_HOST — the board's own legacy Redis, read ONCE to migrate durable state into the state file; default 127.0.0.1 */
    int         redis_port; /* KDESKDASH_REDIS_PORT — default 6379 */
    const char *redis_auth; /* KDESKDASH_CONTROL_REDISCLI_AUTH — NULL when unset (no AUTH). NOT bare REDISCLI_AUTH: that name is the fleet's central-Redis password since sprint 037, and this handle is the board's own passwordless local instance */
    const char *state_path; /* KDESKDASH_STATE_FILE — durable panel state; default /var/lib/kdeskdash/state */
    const char *panel_host; /* the `{host}` segment this panel answers to on central — KDESKDASH_PANEL_HOST, else gethostname()'s first label lowercased; "" disables the command feed */
    const char *cmd_redis_host; /* KDESKDASH_CMD_REDIS_HOST — central control feed (kdash:panelmode/panelshot); falls back to the telemetry values */
    int         cmd_redis_port; /* KDESKDASH_CMD_REDIS_PORT — falls back to telemetry_redis_port */
    const char *cmd_redis_auth; /* KDESKDASH_CMD_REDISCLI_AUTH — falls back to telemetry_redis_auth ONLY when the endpoint is the same instance; NULL otherwise */
    bool        rotate_180; /* KDESKDASH_ROTATE_180 — flip the whole display 180° (case mounts the panel inverted) */
    const char *telemetry_redis_host; /* KDESKDASH_TELEMETRY_REDIS_HOST — kpidash telemetry source, default rpi53 */
    int         telemetry_redis_port; /* KDESKDASH_TELEMETRY_REDIS_PORT — default 6379 */
    const char *telemetry_redis_auth; /* REDISCLI_AUTH — the fleet's central-Redis password from /etc/khomelab/secrets.env; NULL when unset (no AUTH) */
    const char *claude_redis_host; /* KDESKDASH_CLAUDE_REDIS_HOST — claude-feed instance, default 127.0.0.1 (local on rpidash2) */
    int         claude_redis_port; /* KDESKDASH_CLAUDE_REDIS_PORT — default 6380 */
    const char *claude_redis_auth; /* REDISCLI_AUTH — the same central password as telemetry, on its own connection; NULL when unset (no AUTH) */
    const char *kvscf_redis_host;  /* KDESKDASH_KVSCF_REDIS_HOST — foreground/launcher kvscf endpoint; default 127.0.0.1 (no claude-feed inheritance since WI 2305) */
    int         kvscf_redis_port;  /* KDESKDASH_KVSCF_REDIS_PORT — default 6380 */
    const char *kvscf_redis_auth;  /* password for that endpoint, read from the fleet key named by KDESKDASH_KVSCF_REDIS_AUTH_KEY (legacy: KVSCF_REDISCLI_AUTH then CLAUDE_REDISCLI_AUTH); NULL when unset (no AUTH) */
    const char *kvscf_pair_host;   /* KDESKDASH_KVSCF_PAIR_HOST — the one workstation this panel reads and commands; NULL keeps the pre-fold wildcard (CD-8) */
    const char *icons_ttf_path;    /* KDESKDASH_ICONS_TTF — Symbols Nerd Font read at runtime by the icons mode */
    const char *icons_favorites_path; /* KDESKDASH_ICONS_FAVORITES — icons-mode favourites file (load/save) */
    const char *kvscf_token;       /* pairing token for focus/launch/press, read from the fleet key named by KDESKDASH_KVSCF_TOKEN_KEY (deprecated last rung: KVSCF_TOKEN); "" when unset, trimmed at use */
    const char *modes_spec;        /* KDESKDASH_MODES — per-device mode set; NULL when unset (modeset falls back to the full default) */
    const char *card_redis_host;   /* KDESKDASH_CARD_REDIS_HOST — kpidash service-card target; falls back to the telemetry values */
    int         card_redis_port;   /* KDESKDASH_CARD_REDIS_PORT — falls back to telemetry_redis_port */
    const char *card_redis_auth;   /* KDESKDASH_CARD_REDISCLI_AUTH — falls back to telemetry_redis_auth ONLY when the endpoint is the same instance; NULL otherwise */
    const char *quick_pairs;       /* KDESKDASH_QUICK_PAIRS — three states: NULL (unset) = partner is the previously active mode; "none"/"off" = double-tap inert; "a:b,c:d" = pinned pairs. Grammar owned by quickswitch.c */
    const char *card_name;         /* KDESKDASH_CARD_NAME — service-card name segment; default "deskdash" (change it to keep two instances on ONE host distinct) */
} kdeskdash_config_t;

/* Populate cfg from the environment, falling back to defaults. */
void config_load(kdeskdash_config_t *cfg);

#endif /* KDESKDASH_CONFIG_H */
