/**
 * @file redis.c
 * Optional Redis client implementation. Synchronous, single-threaded, driven
 * from the main loop. Connection/reconnect shape mirrors kpidash/src/redis.c:
 * 1s connect timeout, 50ms read timeout, REDISCLI_AUTH -> AUTH, a static
 * context, and reconnect_if_needed() before each poll. All failures are
 * swallowed so the dashboard keeps running when Redis is absent.
 */
#include "redis.h"

#include <hiredis/hiredis.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "shell.h"

#define KEY_ACTIVE_MODE  "kdeskdash:active_mode"
#define KEY_GOL_SETTINGS "kdeskdash:gol:settings"

/* A blocking connect to an unreachable host stalls the single-threaded UI loop
 * for up to the connect timeout. Keep that timeout short and only retry a dead
 * connection every RECONNECT_BACKOFF_S so a missing/remote Redis can't make the
 * panel janky. (Against the default loopback host a down Redis refuses
 * instantly, so this only matters for remote hosts.) */
#define CONNECT_TIMEOUT_MS  250
#define RECONNECT_BACKOFF_S 5

static redisContext *g_ctx = NULL;
static char g_host[128] = "127.0.0.1";
static int  g_port = 6379;
static char g_auth[256] = {0};
static time_t g_next_attempt = 0;

static bool do_connect(void) {
    struct timeval tv = {0, CONNECT_TIMEOUT_MS * 1000};
    redisContext *ctx = redisConnectWithTimeout(g_host, g_port, tv);
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

    if (g_auth[0]) {
        redisReply *r = redisCommand(ctx, "AUTH %s", g_auth);
        bool ok = r && r->type != REDIS_REPLY_ERROR;
        if (r)
            freeReplyObject(r);
        if (!ok) {
            redisFree(ctx);
            return false;
        }
    }

    g_ctx = ctx;
    return true;
}

static bool reconnect_if_needed(void) {
    if (g_ctx && g_ctx->err == 0)
        return true;
    if (g_ctx) {
        redisFree(g_ctx);
        g_ctx = NULL;
    }
    /* Back off after a failed connect so we don't pay the connect timeout on
     * every poll while the host is unreachable. */
    time_t now = time(NULL);
    if (now < g_next_attempt)
        return false;
    if (!do_connect()) {
        g_next_attempt = now + RECONNECT_BACKOFF_S;
        return false;
    }
    return true;
}

void redis_init(const char *host, int port, const char *auth) {
    strncpy(g_host, host ? host : "127.0.0.1", sizeof(g_host) - 1);
    g_host[sizeof(g_host) - 1] = '\0';
    g_port = port > 0 ? port : 6379;
    if (auth) {
        strncpy(g_auth, auth, sizeof(g_auth) - 1);
        g_auth[sizeof(g_auth) - 1] = '\0';
    }
    do_connect(); /* best-effort; a later poll reconnects if this fails */
}

void redis_shutdown(void) {
    if (g_ctx) {
        redisFree(g_ctx);
        g_ctx = NULL;
    }
}

void redis_poll(void) {
    if (!reconnect_if_needed())
        return;

    redisReply *r = redisCommand(g_ctx, "GET %s", KEY_ACTIVE_MODE);
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
    if (!id || !reconnect_if_needed())
        return;
    redisReply *r = redisCommand(g_ctx, "SET %s %s", KEY_ACTIVE_MODE, id);
    if (r)
        freeReplyObject(r);
}

bool redis_get_active_mode(char *buf, size_t buflen) {
    if (!buf || buflen == 0 || !reconnect_if_needed())
        return false;
    redisReply *r = redisCommand(g_ctx, "GET %s", KEY_ACTIVE_MODE);
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
    }
}

bool redis_apply_gol_settings(gol_settings_t *cfg) {
    if (!cfg || !reconnect_if_needed())
        return false;

    redisReply *r = redisCommand(g_ctx, "HGETALL %s", KEY_GOL_SETTINGS);
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
        redisReply *d = redisCommand(g_ctx, "DEL %s", KEY_GOL_SETTINGS);
        if (d)
            freeReplyObject(d);
    }
    return applied;
}
