/**
 * @file redis.c
 * Optional Redis client implementation. Synchronous, single-threaded, driven
 * from the main loop. Connection/reconnect shape mirrors kpidash/src/redis.c:
 * 250 ms connect timeout, 50 ms read timeout, REDISCLI_AUTH -> AUTH, and a
 * reconnect-if-needed gate before each op. All failures are swallowed so the
 * dashboard keeps running when Redis is absent.
 *
 * The connection/backoff machinery lives in a reusable redis_client_t handle so
 * a second endpoint (telemetry) can share the exact same discipline with its
 * own independent context and backoff. The control client below is one such
 * handle (g_control); its public redis_* API is unchanged.
 */
#include "redis.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "shell.h"

#define KEY_ACTIVE_MODE  "kdeskdash:active_mode"
#define KEY_GOL_SETTINGS "kdeskdash:gol:settings"
#define KEY_DEV_LEFT     "kdeskdash:dev:left"
#define KEY_DEV_RIGHT    "kdeskdash:dev:right"

/* A blocking connect to an unreachable host stalls the single-threaded UI loop
 * for up to the connect timeout. Keep that timeout short and only retry a dead
 * connection every RECONNECT_BACKOFF_S so a missing/remote Redis can't make the
 * panel janky. (Against the default loopback host a down Redis refuses
 * instantly, so this only matters for remote hosts.) */
#define CONNECT_TIMEOUT_MS  250
#define RECONNECT_BACKOFF_S 5

/* ---- Generic connection handle (shared by control + telemetry) ---- */

void redis_client_init(redis_client_t *c, const char *host, int port,
                       const char *auth) {
    c->ctx = NULL;
    strncpy(c->host, host ? host : "127.0.0.1", sizeof(c->host) - 1);
    c->host[sizeof(c->host) - 1] = '\0';
    c->port = port > 0 ? port : 6379;
    c->auth[0] = '\0';
    if (auth) {
        strncpy(c->auth, auth, sizeof(c->auth) - 1);
        c->auth[sizeof(c->auth) - 1] = '\0';
    }
    c->next_attempt = 0;
}

bool redis_client_connect(redis_client_t *c) {
    struct timeval tv = {0, CONNECT_TIMEOUT_MS * 1000};
    redisContext *ctx = redisConnectWithTimeout(c->host, c->port, tv);
    if (!ctx || ctx->err) {
        if (ctx)
            redisFree(ctx);
        return false;
    }

    struct timeval poll_tv = {0, 50000}; /* 50ms read timeout */
    if (redisSetTimeout(ctx, poll_tv) != REDIS_OK) {
        redisFree(ctx);
        return false;
    }

    if (c->auth[0]) {
        redisReply *r = redisCommand(ctx, "AUTH %s", c->auth);
        bool ok = r && r->type != REDIS_REPLY_ERROR;
        if (r)
            freeReplyObject(r);
        if (!ok) {
            redisFree(ctx);
            return false;
        }
    }

    c->ctx = ctx;
    return true;
}

bool redis_client_ensure(redis_client_t *c) {
    if (c->ctx && c->ctx->err == 0)
        return true;
    time_t now = time(NULL);
    if (c->ctx) {
        /* The context errored on a *command* (e.g. a read timeout against a
         * reachable-but-slow remote). Reconnecting immediately would thrash
         * connect+timeout every tick and stall the UI thread, so arm the same
         * backoff used for connect failures before dropping it. */
        redisFree(c->ctx);
        c->ctx = NULL;
        c->next_attempt = now + RECONNECT_BACKOFF_S;
    }
    /* Back off after a failed connect so we don't pay the connect timeout on
     * every op while the host is unreachable. */
    if (now < c->next_attempt)
        return false;
    if (!redis_client_connect(c)) {
        c->next_attempt = now + RECONNECT_BACKOFF_S;
        return false;
    }
    return true;
}

void redis_client_close(redis_client_t *c) {
    if (c->ctx) {
        redisFree(c->ctx);
        c->ctx = NULL;
    }
}

/* ---- Control client (local endpoint): unchanged public API ---- */

static redis_client_t g_control;

void redis_init(const char *host, int port, const char *auth) {
    redis_client_init(&g_control, host, port, auth);
    redis_client_connect(&g_control); /* best-effort; a later op reconnects */
}

void redis_shutdown(void) {
    redis_client_close(&g_control);
}

void redis_poll(void) {
    if (!redis_client_ensure(&g_control))
        return;

    redisReply *r = redisCommand(g_control.ctx, "GET %s", KEY_ACTIVE_MODE);
    if (!r) {
        /* Context is now in error; next poll reconnects. */
        return;
    }
    if (r->type == REDIS_REPLY_STRING && r->len > 0) {
        kd_mode_t *m = shell_find_mode(r->str);
        if (m && m != shell_active())
            shell_set_active(m); /* unknown ids are simply ignored */
    }
    freeReplyObject(r);
}

void redis_set_active_mode(const char *id) {
    if (!id || !redis_client_ensure(&g_control))
        return;
    redisReply *r = redisCommand(g_control.ctx, "SET %s %s", KEY_ACTIVE_MODE, id);
    if (r)
        freeReplyObject(r);
}

/* GET `key` into buf (truncated, always NUL-terminated). False on any error,
 * missing key, wrong type, or empty value; buf is left untouched on failure. */
static bool redis_get_string(const char *key, char *buf, size_t buflen) {
    if (!buf || buflen == 0 || !redis_client_ensure(&g_control))
        return false;
    redisReply *r = redisCommand(g_control.ctx, "GET %s", key);
    bool ok = false;
    if (r && r->type == REDIS_REPLY_STRING && r->len > 0) {
        strncpy(buf, r->str, buflen - 1);
        buf[buflen - 1] = '\0';
        ok = true;
    }
    if (r)
        freeReplyObject(r);
    return ok;
}

bool redis_get_active_mode(char *buf, size_t buflen) {
    return redis_get_string(KEY_ACTIVE_MODE, buf, buflen);
}

static const char *dev_side_key(redis_dev_side_t side) {
    return side == REDIS_DEV_SIDE_LEFT ? KEY_DEV_LEFT : KEY_DEV_RIGHT;
}

void redis_set_dev_assignment(redis_dev_side_t side, const char *host) {
    if (!redis_client_ensure(&g_control))
        return;
    const char *key = dev_side_key(side);
    redisReply *r;
    if (host && host[0] != '\0')
        r = redisCommand(g_control.ctx, "SET %s %s", key, host);
    else
        r = redisCommand(g_control.ctx, "DEL %s", key);
    if (r)
        freeReplyObject(r);
}

bool redis_get_dev_assignment(redis_dev_side_t side, char *buf, size_t buflen) {
    return redis_get_string(dev_side_key(side), buf, buflen);
}

/* Apply one "field value" pair from the settings hash onto cfg. These are
 * hard safety bounds, intentionally wider than random_settings() in
 * game_of_life.c so a remote client can experiment — except the cell_size
 * floor, which must stay >= 2 to preserve the bounded worst-case grid / per-
 * frame work that random_settings() also enforces. */
static void apply_field(gol_settings_t *cfg, const char *field, const char *val) {
    if (strcmp(field, "cell_size") == 0) {
        int v = atoi(val);
        if (v >= 2 && v <= 64)
            cfg->cell_size = v;
    } else if (strcmp(field, "padding") == 0) {
        int v = atoi(val);
        if (v >= 0 && v <= 16)
            cfg->padding = v;
    } else if (strcmp(field, "density") == 0) {
        double v = atof(val);
        if (v > 0.0 && v <= 1.0)
            cfg->density = v;
    } else if (strcmp(field, "trail") == 0) {
        cfg->trail = atoi(val) != 0;
    } else if (strcmp(field, "trail_turns") == 0) {
        int v = atoi(val);
        if (v >= 1 && v <= 64)
            cfg->trail_turns = v;
    } else if (strcmp(field, "speed_ms") == 0) {
        int v = atoi(val);
        if (v >= 10 && v <= 5000)
            cfg->speed_ms = v;
    } else if (strcmp(field, "rgb") == 0) {
        cfg->rgb = atoi(val) != 0;
    }
}

bool redis_apply_gol_settings(gol_settings_t *cfg) {
    if (!cfg || !redis_client_ensure(&g_control))
        return false;

    redisReply *r = redisCommand(g_control.ctx, "HGETALL %s", KEY_GOL_SETTINGS);
    bool applied = false;
    if (r && r->type == REDIS_REPLY_ARRAY && r->elements >= 2) {
        for (size_t i = 0; i + 1 < r->elements; i += 2) {
            redisReply *k = r->element[i];
            redisReply *v = r->element[i + 1];
            if (k && v && k->type == REDIS_REPLY_STRING &&
                v->type == REDIS_REPLY_STRING) {
                apply_field(cfg, k->str, v->str);
                applied = true;
            }
        }
    }
    if (r)
        freeReplyObject(r);

    if (applied) {
        redisReply *d = redisCommand(g_control.ctx, "DEL %s", KEY_GOL_SETTINGS);
        if (d)
            freeReplyObject(d);
    }
    return applied;
}
