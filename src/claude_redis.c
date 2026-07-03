/**
 * @file claude_redis.c
 * Read-only claude-feed client on a dedicated redis_client_t handle.
 */
#include "claude_redis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "redis.h"
#include "telemetry_host.h" /* token contract shared with the telemetry feed */

#define CLAUDE_SCAN_MATCH CF_KEY_PREFIX "*"
#define CLAUDE_SCAN_COUNT 64
/* A session hash is a handful of short fields; a limits hash smaller still. */
#define CLAUDE_FIELD_MAX  16
#define CLAUDE_VALUE_MAX  512
#define CLAUDE_RECORD_MAX 1024 /* one claude:recent JSON record */

static redis_client_t g_cf;

/* Discovery state: `building` accumulates across one SCAN pass and is
 * published atomically into `current` on cursor wrap (telemetry.c pattern). */
static unsigned long long g_cursor;
static claude_key_t g_building[CF_SESSIONS_MAX];
static int g_building_n;
static claude_key_t g_current[CF_SESSIONS_MAX];
static int g_current_n;

static bool g_reachable;

void claude_redis_init(const char *host, int port, const char *auth) {
    redis_client_init(&g_cf, host, port, auth);
    g_cursor = 0;
    g_building_n = 0;
    g_current_n = 0;
    g_reachable = false;
}

void claude_redis_shutdown(void) {
    redis_client_close(&g_cf);
    g_cursor = 0;
    g_building_n = 0;
    g_current_n = 0;
    g_reachable = false;
}

bool claude_redis_reachable(void) {
    return g_reachable;
}

static void building_add(const char *host, const char *sid) {
    if (g_building_n >= CF_SESSIONS_MAX)
        return;
    for (int i = 0; i < g_building_n; i++)
        if (strcmp(g_building[i].host, host) == 0 &&
            strcmp(g_building[i].sid, sid) == 0)
            return;
    snprintf(g_building[g_building_n].host, CF_HOST_MAX, "%s", host);
    snprintf(g_building[g_building_n].sid, CF_SID_MAX, "%s", sid);
    g_building_n++;
}

static void publish_building(void) {
    memcpy(g_current, g_building, sizeof(g_current));
    g_current_n = g_building_n;
    g_building_n = 0;
    g_cursor = 0;
}

bool claude_redis_discover_step(void) {
    if (!redis_client_ensure(&g_cf)) {
        g_reachable = false;
        g_cursor = 0;
        g_building_n = 0;
        return false;
    }

    char curbuf[32];
    snprintf(curbuf, sizeof(curbuf), "%llu", g_cursor);
    redisReply *r = redisCommand(g_cf.ctx, "SCAN %s MATCH %s COUNT %d", curbuf,
                                 CLAUDE_SCAN_MATCH, CLAUDE_SCAN_COUNT);
    if (!r) {
        g_reachable = false;
        g_cursor = 0;
        g_building_n = 0;
        return false;
    }
    g_reachable = true;

    if (r->type != REDIS_REPLY_ARRAY || r->elements != 2 ||
        r->element[0]->type != REDIS_REPLY_STRING) {
        freeReplyObject(r);
        publish_building();
        return true;
    }

    g_cursor = strtoull(r->element[0]->str, NULL, 10);

    redisReply *keys = r->element[1];
    if (keys->type == REDIS_REPLY_ARRAY) {
        for (size_t i = 0; i < keys->elements; i++) {
            redisReply *k = keys->element[i];
            if (k->type != REDIS_REPLY_STRING)
                continue;
            char host[CF_HOST_MAX], sid[CF_SID_MAX];
            /* Choke point: only contract-clean keys survive discovery. */
            if (cf_key_parse(k->str, (size_t)k->len, host, sizeof(host), sid,
                             sizeof(sid)))
                building_add(host, sid);
        }
    }
    freeReplyObject(r);

    if (g_cursor == 0 || g_building_n >= CF_SESSIONS_MAX) {
        publish_building();
        return true;
    }
    return false;
}

int claude_redis_keys(claude_key_t *out, int max) {
    int n = (g_current_n < max) ? g_current_n : max;
    for (int i = 0; i < n; i++)
        out[i] = g_current[i];
    return n;
}

/* Flatten an HGETALL reply (2N bulk strings) into bounded field/value pointer
 * arrays. Oversized values are skipped rather than truncated-and-trusted.
 * Returns the pair count (0 for an empty/missing hash). */
static int reply_to_pairs(redisReply *r, const char *fields[], const char *values[],
                          int max) {
    if (r->type != REDIS_REPLY_ARRAY || r->elements < 2)
        return 0;
    int n = 0;
    for (size_t i = 0; i + 1 < r->elements && n < max; i += 2) {
        redisReply *f = r->element[i];
        redisReply *v = r->element[i + 1];
        if (f->type != REDIS_REPLY_STRING || v->type != REDIS_REPLY_STRING)
            continue;
        if (f->len == 0 || (size_t)v->len > CLAUDE_VALUE_MAX)
            continue;
        fields[n] = f->str;
        values[n] = v->str;
        n++;
    }
    return n;
}

claude_status_t claude_redis_get_session(const claude_key_t *k, cf_session_t *out) {
    memset(out, 0, sizeof(*out));
    /* Tokens came through cf_key_parse, but re-validate at the fetch boundary
     * (same discipline as telemetry_get_sample). */
    if (!k || !telemetry_host_token_ok(k->host, strlen(k->host)) ||
        !telemetry_host_token_ok(k->sid, strlen(k->sid)))
        return CLAUDE_ABSENT;

    if (!redis_client_ensure(&g_cf)) {
        g_reachable = false;
        return CLAUDE_UNAVAIL;
    }

    char key[sizeof(CF_KEY_PREFIX) + CF_HOST_MAX + CF_SID_MAX];
    snprintf(key, sizeof(key), CF_KEY_PREFIX "%s:%s", k->host, k->sid);

    redisReply *r = redisCommand(g_cf.ctx, "HGETALL %s", key);
    if (!r) {
        g_reachable = false;
        return CLAUDE_UNAVAIL;
    }
    g_reachable = true;

    const char *fields[CLAUDE_FIELD_MAX];
    const char *values[CLAUDE_FIELD_MAX];
    int n = reply_to_pairs(r, fields, values, CLAUDE_FIELD_MAX);

    claude_status_t status = CLAUDE_ABSENT;
    if (n > 0 && cf_session_from_fields(k->host, k->sid, fields, values, n, out))
        status = CLAUDE_OK;
    freeReplyObject(r);
    return status;
}

claude_status_t claude_redis_get_limits(cf_limits_t *out) {
    memset(out, 0, sizeof(*out));

    if (!redis_client_ensure(&g_cf)) {
        g_reachable = false;
        return CLAUDE_UNAVAIL;
    }

    redisReply *r = redisCommand(g_cf.ctx, "HGETALL claude:limits");
    if (!r) {
        g_reachable = false;
        return CLAUDE_UNAVAIL;
    }
    g_reachable = true;

    const char *fields[CLAUDE_FIELD_MAX];
    const char *values[CLAUDE_FIELD_MAX];
    int n = reply_to_pairs(r, fields, values, CLAUDE_FIELD_MAX);

    claude_status_t status = CLAUDE_ABSENT;
    if (n > 0 && cf_limits_from_fields(fields, values, n, out))
        status = CLAUDE_OK;
    freeReplyObject(r);
    return status;
}

int claude_redis_get_recent(cf_recent_t *out, int max) {
    if (max <= 0)
        return 0;

    if (!redis_client_ensure(&g_cf)) {
        g_reachable = false;
        return 0;
    }

    redisReply *r = redisCommand(g_cf.ctx, "LRANGE claude:recent 0 %d", max - 1);
    if (!r) {
        g_reachable = false;
        return 0;
    }
    g_reachable = true;

    int n = 0;
    if (r->type == REDIS_REPLY_ARRAY) {
        for (size_t i = 0; i < r->elements && n < max; i++) {
            redisReply *e = r->element[i];
            if (e->type != REDIS_REPLY_STRING || e->len == 0 ||
                (size_t)e->len > CLAUDE_RECORD_MAX)
                continue;
            if (cf_recent_parse(e->str, (size_t)e->len, &out[n]))
                n++;
        }
    }
    freeReplyObject(r);
    return n;
}
