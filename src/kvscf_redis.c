/**
 * @file kvscf_redis.c
 * kvscf window-feed client on a dedicated redis_client_t handle (6380). Mirrors
 * claude_redis.c: lazy connect, swallowed failures reported as "unavailable".
 */
#include "kvscf_redis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "redis.h"
#include "redis_internal.h" /* embeds a redis_client_t by value + touches .ctx */
#include "telemetry_host.h" /* host token contract (shared choke point) */

#define KV_SCAN_MATCH "kvscf:instances:*"
#define KV_EDGE_MATCH "kvscf:edge:*"
#define KV_SCAN_COUNT 64
#define KV_MAX_HOSTS  8      /* distinct publisher hosts (only cleo today) */
#define KV_KEY_MAX    96     /* "kvscf:instances:" + host */
#define KV_VALUE_MAX  32768  /* upper bound on one host's instance JSON */

static redis_client_t g_kv;
static bool           g_reachable;
static char           g_token[KV_TOKEN_MAX];

void kvscf_redis_init(const char *host, int port, const char *auth,
                      const char *token) {
    redis_client_init(&g_kv, host, port, auth);
    g_reachable = false;
    snprintf(g_token, sizeof(g_token), "%s", token ? token : "");
    kvscf_trim_trailing(g_token); /* byte-exact match — kill any CR/LF/space */
}

void kvscf_redis_shutdown(void) {
    redis_client_close(&g_kv);
    g_reachable = false;
    /* Clear the secret from memory on teardown. */
    memset(g_token, 0, sizeof(g_token));
}

bool kvscf_redis_reachable(void) {
    return g_reachable;
}

bool kvscf_redis_have_token(void) {
    return g_token[0] != '\0';
}

/* Collect keys matching `match` via a bounded SCAN loop (few keys — one host
 * today; guard caps iterations so a pathological cursor can't spin). Requires an
 * ensured ctx. Returns the key count, or -1 if the endpoint drops mid-scan. */
static int scan_keys(const char *match, char keys[][KV_KEY_MAX], int maxkeys) {
    int nkeys = 0;
    unsigned long long cursor = 0;
    int guard = 0;
    do {
        redisReply *r = redisCommand(g_kv.ctx, "SCAN %llu MATCH %s COUNT %d",
                                     cursor, match, KV_SCAN_COUNT);
        if (!r) {
            g_reachable = false;
            return -1;
        }
        g_reachable = true;
        if (r->type != REDIS_REPLY_ARRAY || r->elements != 2 ||
            r->element[0]->type != REDIS_REPLY_STRING) {
            freeReplyObject(r);
            break;
        }
        cursor = strtoull(r->element[0]->str, NULL, 10);
        redisReply *ks = r->element[1];
        if (ks->type == REDIS_REPLY_ARRAY) {
            for (size_t i = 0; i < ks->elements && nkeys < maxkeys; i++) {
                redisReply *k = ks->element[i];
                if (k->type == REDIS_REPLY_STRING && k->len > 0 &&
                    (size_t)k->len < KV_KEY_MAX) {
                    memcpy(keys[nkeys], k->str, (size_t)k->len);
                    keys[nkeys][k->len] = '\0';
                    nkeys++;
                }
            }
        }
        freeReplyObject(r);
    } while (cursor != 0 && ++guard < 16 && nkeys < maxkeys);
    return nkeys;
}

int kvscf_redis_refresh(kvscf_instance_t *out, int max) {
    if (!out || max <= 0)
        return 0;
    if (!redis_client_ensure(&g_kv)) {
        g_reachable = false;
        return 0;
    }
    char keys[KV_MAX_HOSTS][KV_KEY_MAX];
    int nkeys = scan_keys(KV_SCAN_MATCH, keys, KV_MAX_HOSTS);
    if (nkeys < 0)
        return 0;

    int count = 0;
    for (int i = 0; i < nkeys && count < max; i++) {
        redisReply *g = redisCommand(g_kv.ctx, "GET %s", keys[i]);
        if (!g) {
            g_reachable = false;
            break;
        }
        if (g->type == REDIS_REPLY_STRING && g->len > 0 &&
            (size_t)g->len <= KV_VALUE_MAX)
            count = kvscf_parse_append(g->str, (size_t)g->len, out, count, max);
        freeReplyObject(g);
    }
    kvscf_sort_by_label(out, count);
    return count;
}

int kvscf_redis_refresh_edge(kvscf_edge_t *out, int max) {
    if (!out || max <= 0)
        return 0;
    if (!redis_client_ensure(&g_kv)) {
        g_reachable = false;
        return 0;
    }
    char keys[KV_MAX_HOSTS][KV_KEY_MAX];
    int nkeys = scan_keys(KV_EDGE_MATCH, keys, KV_MAX_HOSTS);
    if (nkeys < 0)
        return 0;

    int count = 0;
    for (int i = 0; i < nkeys && count < max; i++) {
        redisReply *g = redisCommand(g_kv.ctx, "GET %s", keys[i]);
        if (!g) {
            g_reachable = false;
            break;
        }
        if (g->type == REDIS_REPLY_STRING && g->len > 0 &&
            (size_t)g->len <= KV_VALUE_MAX)
            count = kvscf_parse_edge_append(g->str, (size_t)g->len, out, count, max);
        freeReplyObject(g);
    }
    kvscf_sort_edge(out, count);
    return count;
}

bool kvscf_redis_focus(const char *host, const char *id, bool maximize) {
    /* R8: never send an unauthenticated command. */
    if (g_token[0] == '\0')
        return false;
    if (!host || !telemetry_host_token_ok(host, strlen(host)))
        return false;

    char payload[KV_TOKEN_MAX + 128];
    if (kvscf_focus_payload(g_token, id, maximize, payload, sizeof(payload)) == 0)
        return false;

    if (!redis_client_ensure(&g_kv)) {
        g_reachable = false;
        return false;
    }

    char chan[16 + KV_HOST_MAX];
    snprintf(chan, sizeof(chan), "kvscf:focus:%s", host);
    /* hiredis %s sends each arg as one binary-safe bulk string, so the JSON
     * payload's spaces/quotes need no manual escaping. */
    redisReply *r = redisCommand(g_kv.ctx, "PUBLISH %s %s", chan, payload);
    if (!r) {
        g_reachable = false;
        return false;
    }
    g_reachable = true;
    freeReplyObject(r);
    return true;
}
