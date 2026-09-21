/**
 * @file panel_feed.c
 * libkdash glue for the central control cluster. See panel_feed.h for the
 * contract; the policy this file defers to (which `{host}`, which screenshot
 * paths) is pure and lives in panel_cmd.h.
 */
#include "panel_feed.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "gol_settings.h"
#include "kdash/kdash_conn.h"
#include "kdash/kdash_endpoint.h"
#include "kdash/kdash_feed.h"
#include "kdash/kdash_freshness.h"
#include "kdash/kdash_payload.h"
#include "panel_cmd.h"
#include "screenshot.h"
#include "shell.h"

static kdash_conn_t *g_conn;
static char          g_host[PANEL_CMD_HOST_MAX];

/* One acted stamp PER VERB (kdash_feed.h): a screenshot and a mode switch are
 * separate commands with separate edges. Sharing one would make acting on
 * either suppress the next of the other kind. In memory only — a restart
 * starts at 0, and the window is what keeps that safe. */
static double g_mode_ts;
static double g_shot_ts;

/* The settings the last actionable mode command carried, and one consume flag
 * per vocabulary. See panel_feed.h for why there are two flags over one set. */
static kdash_panelmode_t g_pending;
static bool              g_pending_gol;
static bool              g_pending_golz;

void panel_feed_init(const char *host, int port, const char *auth,
                     const char *panel_host) {
    g_conn = NULL;
    g_host[0] = '\0';
    g_mode_ts = 0;
    g_shot_ts = 0;
    g_pending_gol = false;
    g_pending_golz = false;

    if (!panel_host || panel_host[0] == '\0') {
        fprintf(stderr, "kdeskdash: no panel host name — central commands "
                        "disabled (set KDESKDASH_PANEL_HOST)\n");
        return;
    }
    snprintf(g_host, sizeof(g_host), "%s", panel_host);

    kdash_conn_opts_t opts = {
        .app = "kdeskdash",
        /* The CENTRAL stem, not the claude one: `kdash:*` is a central family
         * (kdash_feed.h). Two stems answering the same host:port today is
         * exactly the coincidence a stem exists to survive. */
        .stem = &KDASH_STEM_CENTRAL,
        /* Explicit endpoint beats khlenv discovery (CD-4): the panel's env
         * file pins where central is, and an appliance should not depend on a
         * resolution path it cannot see. */
        .host = host,
        .port = port,
        /* libkdash reads $REDISCLI_AUTH when `auth` is NULL. That variable IS
         * central's password on every fleet host since sprint 037, so leaving
         * it NULL would usually work and would be an accident either way —
         * "" is how this project says "no AUTH" (see modes/claude.c and
         * docs/solutions/best-practices/adopting-a-library-inherits-its-defaults.md). */
        .auth = auth ? auth : "",
    };
    g_conn = kdash_conn_new(&opts);
    if (!g_conn) {
        fprintf(stderr, "kdeskdash: could not open the central command feed\n");
        return;
    }
    printf("kdeskdash: central commands as \"%s\" (%s:%d)\n", g_host,
           host ? host : "(resolved)", port);
}

void panel_feed_shutdown(void) {
    kdash_conn_free(g_conn);
    g_conn = NULL;
}

/* Act on a mode command: stash its settings (so the mode's own reseed picks
 * them up on activation, exactly as it picked up the Redis hash) and switch. */
static void act_mode(const kdash_panelmode_t *cmd) {
    if (cmd->settings_count > 0) {
        g_pending = *cmd;
        g_pending_gol = true;
        g_pending_golz = true;
    }
    if (cmd->settings_truncated || cmd->settings_skipped > 0)
        fprintf(stderr,
                "kdeskdash: mode command for \"%s\" had %d unreadable "
                "setting(s)%s\n",
                cmd->mode, cmd->settings_skipped,
                cmd->settings_truncated ? " and more than fit" : "");

    kd_mode_t *m = shell_find_mode(cmd->mode);
    if (!m) {
        /* The vocabulary is the dashboard's (CD-22) and this panel's mode set
         * is per-device, so an id this board does not carry is an ordinary
         * outcome — say so once rather than ignoring it silently, because the
         * symptom otherwise is a command that appears to do nothing. */
        fprintf(stderr, "kdeskdash: central asked for mode \"%s\", which this "
                        "panel does not have — ignored\n",
                cmd->mode);
        return;
    }
    /* Only when it is a different mode, which is what the active_mode poll did.
     * A repeat of the current mode leaves the pending settings for that mode's
     * next activation — again, what the Redis hash did by simply sitting there. */
    if (m != shell_active())
        shell_set_active(m);
}

static void act_shot(const kdash_panelshot_t *cmd) {
    if (cmd->path[0] == '\0') {
        screenshot_save(NULL); /* absent path: the panel's own default */
        return;
    }
    if (!panel_cmd_shot_path_ok(cmd->path)) {
        /* This key is writable by every holder of the central password
         * (CD-23), so the directory policy is ours and a refusal has to be
         * visible — a root process silently declining to write where it was
         * told is the one outcome nobody can debug from the outside. */
        fprintf(stderr,
                "kdeskdash: refusing screenshot path \"%s\" — outside %s\n",
                cmd->path, PANEL_CMD_SHOT_DIR);
        return;
    }
    screenshot_save(cmd->path);
}

void panel_feed_poll(void) {
    if (!g_conn || g_host[0] == '\0')
        return;

    long long now = (long long)time(NULL);

    kdash_panelmode_t mode;
    if (kdash_panelmode(g_conn, g_host, &mode) == KDASH_OK &&
        kdash_cmd_actionable(mode.ts, g_mode_ts, now, KDASH_PANEL_WINDOW_S)) {
        g_mode_ts = mode.ts;
        act_mode(&mode);
    }

    kdash_panelshot_t shot;
    if (kdash_panelshot(g_conn, g_host, &shot) == KDASH_OK &&
        kdash_cmd_actionable(shot.ts, g_shot_ts, now, KDASH_PANEL_WINDOW_S)) {
        g_shot_ts = shot.ts;
        act_shot(&shot);
    }
}

/* Walk the pending settings once, handing each name to `apply`. The applier
 * owns validation and ignores names it does not know, so the two vocabularies
 * can share one `settings` object without either guessing at the other's. */
static bool consume(bool *flag, bool (*apply)(void *, const char *, const char *),
                    void *cfg) {
    if (!*flag || !cfg)
        return false;
    *flag = false;
    bool applied = false;
    for (int i = 0; i < g_pending.settings_count; i++) {
        if (apply(cfg, g_pending.settings[i].name, g_pending.settings[i].value))
            applied = true;
    }
    return applied;
}

static bool apply_gol(void *cfg, const char *name, const char *val) {
    return gol_settings_apply_field((gol_settings_t *)cfg, name, val);
}

static bool apply_golz(void *cfg, const char *name, const char *val) {
    return golz_settings_apply_field((golz_settings_t *)cfg, name, val);
}

bool panel_feed_apply_gol_settings(gol_settings_t *cfg) {
    return consume(&g_pending_gol, apply_gol, cfg);
}

bool panel_feed_apply_golz_settings(golz_settings_t *cfg) {
    return consume(&g_pending_golz, apply_golz, cfg);
}
