/**
 * @file panel_store.c
 * Process-wide, write-through store over the durable panel state file, plus
 * the one-time migration out of the legacy control Redis. See panel_store.h.
 *
 * The contract logic is all in the pure panel_state core; this file is the
 * singleton, the write-through discipline, and the only place left in the
 * program that dials the board's own Redis — once, on first run.
 */
#include "panel_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "panel_state.h"
#include "redis.h"
#include "redis_internal.h" /* the migration embeds a redis_client_t by value */

/* Floor for the adaptive human-win threshold, mirroring the game rule. It sat
 * in redis.c beside the getter it guarded; it moves with it. */
#define GOLZ_GENS_FLOOR 100

/* The legacy key names. Nothing writes them any more and the migration is the
 * only reader, but they are the whole record of what the panel used to keep in
 * Redis — so they stay named here rather than becoming a mystery HGETALL. */
#define KEY_ACTIVE_MODE      "kdeskdash:active_mode"
#define KEY_GOLZ_WINS        "kdeskdash:golz:wins" /* legacy: historical zombie wins */
#define KEY_GOLZ_HUMAN_WINS  "kdeskdash:golz:human_wins"
#define KEY_GOLZ_ZOMBIE_WINS "kdeskdash:golz:zombie_wins"
#define KEY_GOLZ_TIES        "kdeskdash:golz:ties"
#define KEY_GOLZ_GENS_TO_WIN "kdeskdash:golz:gens_to_win"
#define KEY_DEV_LEFT         "kdeskdash:dev:left"
#define KEY_DEV_RIGHT        "kdeskdash:dev:right"
#define KEY_CALC_REGS        "kdeskdash:calc:regs"

static panel_state_t g_state;
static char          g_path[512];
static bool          g_ready;    /* init has run; before that every op no-ops */
static bool          g_warned;   /* a write has already failed and been reported */

/* --- write-through -------------------------------------------------------
 *
 * Failure is reported ONCE. A panel whose state directory went read-only would
 * otherwise print a line per swipe forever, and the interesting event is the
 * first one. The in-memory copy stays authoritative for the rest of the run,
 * so the panel behaves exactly as it did with Redis unreachable. */
static bool commit(void) {
    if (!g_ready)
        return false;
    if (panel_state_save(g_path, &g_state))
        return true;
    if (!g_warned) {
        g_warned = true;
        fprintf(stderr,
                "kdeskdash: cannot write panel state to %s — counters and "
                "registers will not survive a restart\n",
                g_path);
    }
    return false;
}

static void set_str(char *dst, size_t dstsz, const char *src) {
    if (!src || src[0] == '\0') {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dstsz, "%s", src);
}

static bool get_str(const char *src, char *buf, size_t buflen) {
    if (!buf || buflen == 0 || src[0] == '\0')
        return false;
    snprintf(buf, buflen, "%s", src);
    return true;
}

/* --- the one-time migration ---------------------------------------------- */

/* GET `key` as a non-negative count, or leave *out alone. Absent, wrong type
 * and unparseable are all "nothing to copy", which is the common case for a
 * board that never played GoLZ. */
static void migrate_count(redis_client_t *c, const char *key, long *out) {
    redisReply *r = redisCommand(c->ctx, "GET %s", key);
    if (r && r->type == REDIS_REPLY_STRING && r->len > 0) {
        char *end = NULL;
        long v = strtol(r->str, &end, 10);
        if (end && *end == '\0' && v >= 0)
            *out = v;
    }
    if (r)
        freeReplyObject(r);
}

static void migrate_str(redis_client_t *c, const char *key, char *dst,
                        size_t dstsz) {
    redisReply *r = redisCommand(c->ctx, "GET %s", key);
    if (r && r->type == REDIS_REPLY_STRING && r->len > 0)
        snprintf(dst, dstsz, "%s", r->str);
    if (r)
        freeReplyObject(r);
}

/* Copy whatever the legacy control Redis still holds into `g_state`. COPY —
 * nothing is deleted, so rolling back to a build that still reads Redis finds
 * its state untouched. Best-effort throughout: an unreachable endpoint means a
 * panel that starts with fresh counters, which is the correct outcome for a
 * board that never had them. */
static void migrate_from_redis(const char *host, int port, const char *auth) {
    redis_client_t legacy;
    redis_client_init(&legacy, host, port, auth);
    if (!redis_client_connect(&legacy)) {
        printf("kdeskdash: no legacy Redis at %s:%d — starting with fresh "
               "panel state\n",
               host ? host : "127.0.0.1", port);
        redis_client_close(&legacy);
        return;
    }

    migrate_count(&legacy, KEY_GOLZ_HUMAN_WINS, &g_state.golz_human_wins);
    migrate_count(&legacy, KEY_GOLZ_ZOMBIE_WINS, &g_state.golz_zombie_wins);
    migrate_count(&legacy, KEY_GOLZ_TIES, &g_state.golz_ties);
    migrate_count(&legacy, KEY_GOLZ_GENS_TO_WIN, &g_state.golz_gens_to_win);
    migrate_count(&legacy, KEY_GOLZ_WINS, &g_state.golz_wins);
    migrate_str(&legacy, KEY_CALC_REGS, g_state.calc_regs,
                sizeof(g_state.calc_regs));
    migrate_str(&legacy, KEY_DEV_LEFT, g_state.dev_left,
                sizeof(g_state.dev_left));
    migrate_str(&legacy, KEY_DEV_RIGHT, g_state.dev_right,
                sizeof(g_state.dev_right));
    migrate_str(&legacy, KEY_ACTIVE_MODE, g_state.active_mode,
                sizeof(g_state.active_mode));

    redis_client_close(&legacy);
    printf("kdeskdash: migrated panel state from %s:%d (copied, not moved)\n",
           host ? host : "127.0.0.1", port);
}

void panel_store_init(const char *path, const char *legacy_host,
                      int legacy_port, const char *legacy_auth) {
    snprintf(g_path, sizeof(g_path), "%s",
             (path && path[0] != '\0') ? path : PANEL_STATE_DEFAULT_PATH);
    g_warned = false;
    g_ready = true;

    switch (panel_state_load(g_path, &g_state)) {
    case PANEL_STATE_OK:
        printf("kdeskdash: panel state loaded from %s\n", g_path);
        return;
    case PANEL_STATE_ABSENT:
        /* First run on this board: this is the migration window, and the only
         * one. Once the file exists it is the authority. */
        if (legacy_host)
            migrate_from_redis(legacy_host, legacy_port, legacy_auth);
        commit();
        return;
    case PANEL_STATE_INVALID:
        /* Rejected whole, the calc:regs rule. Deliberately does NOT migrate:
         * the Redis values are stale by now, and restoring them would be
         * indistinguishable from the file having been fine. */
        fprintf(stderr,
                "kdeskdash: panel state at %s is malformed — rejected whole, "
                "starting from defaults (it will be overwritten on the next "
                "change)\n",
                g_path);
        return;
    case PANEL_STATE_IO:
    default:
        fprintf(stderr, "kdeskdash: cannot read panel state at %s — starting "
                        "from defaults\n",
                g_path);
        return;
    }
}

/* --- counters ------------------------------------------------------------ */

static long incr(long *field) {
    if (!g_ready)
        return -1;
    /* An unset counter increments from zero, so a fresh panel's first win
     * reads 1 — the same answer Redis's INCR gave on a missing key. */
    *field = (*field < 0 ? 0 : *field) + 1;
    commit(); /* the count is real whether or not the write lands */
    return *field;
}

static long get(long field, long default_val) {
    return (g_ready && field >= 0) ? field : default_val;
}

long panel_store_golz_incr_human_wins(void) {
    return incr(&g_state.golz_human_wins);
}

long panel_store_golz_incr_zombie_wins(void) {
    return incr(&g_state.golz_zombie_wins);
}

long panel_store_golz_incr_ties(void) {
    return incr(&g_state.golz_ties);
}

long panel_store_golz_get_human_wins(long default_val) {
    return get(g_state.golz_human_wins, default_val);
}

long panel_store_golz_get_zombie_wins(long default_val) {
    return get(g_state.golz_zombie_wins, default_val);
}

long panel_store_golz_get_ties(long default_val) {
    return get(g_state.golz_ties, default_val);
}

long panel_store_golz_get_wins(long default_val) {
    return get(g_state.golz_wins, default_val);
}

long panel_store_golz_get_gens_to_win(long default_val) {
    long v = get(g_state.golz_gens_to_win, -1);
    if (v < 0)
        return default_val;
    return v < GOLZ_GENS_FLOOR ? GOLZ_GENS_FLOOR : v;
}

long panel_store_golz_set_gens_to_win(long value) {
    if (value < GOLZ_GENS_FLOOR)
        value = GOLZ_GENS_FLOOR; /* enforce the game-rule floor */
    if (g_ready) {
        g_state.golz_gens_to_win = value;
        commit();
    }
    return value; /* the floored value either way, so the caller can mirror it */
}

/* --- strings ------------------------------------------------------------- */

void panel_store_set_calc_regs(const char *line) {
    if (!g_ready)
        return;
    set_str(g_state.calc_regs, sizeof(g_state.calc_regs), line);
    commit();
}

bool panel_store_get_calc_regs(char *buf, size_t buflen) {
    return g_ready && get_str(g_state.calc_regs, buf, buflen);
}

static char *dev_slot(panel_dev_side_t side) {
    return side == PANEL_DEV_SIDE_LEFT ? g_state.dev_left : g_state.dev_right;
}

void panel_store_set_dev_assignment(panel_dev_side_t side, const char *host) {
    if (!g_ready)
        return;
    set_str(dev_slot(side), PANEL_STATE_HOST_MAX, host);
    commit();
}

bool panel_store_get_dev_assignment(panel_dev_side_t side, char *buf,
                                    size_t buflen) {
    return g_ready && get_str(dev_slot(side), buf, buflen);
}

void panel_store_set_active_mode(const char *id) {
    if (!g_ready)
        return;
    /* The shell calls this on every change, including ones that did not change
     * the id (a restore writing back what it read). Skip the identical write
     * so a swipe cycle that returns to where it started costs no file I/O. */
    if (id && strcmp(g_state.active_mode, id) == 0)
        return;
    set_str(g_state.active_mode, sizeof(g_state.active_mode), id);
    commit();
}

bool panel_store_get_active_mode(char *buf, size_t buflen) {
    return g_ready && get_str(g_state.active_mode, buf, buflen);
}
