/**
 * @file kvscf_redis.c
 * kvscf window-feed client on a dedicated redis_client_t handle (6380): lazy
 * connect, swallowed failures reported as "unavailable". The claude feed was
 * this shape too until sprint 034 moved it onto libkdash's own client.
 */
#include "kvscf_redis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "redis.h"
#include "redis_internal.h" /* embeds a redis_client_t by value + touches .ctx */
#include "telemetry_host.h" /* host token contract (shared choke point) */

/* Key-family PREFIXES, not finished patterns: the trailing segment is either
 * the configured pair host or `*`, decided per call by kvscf_scan_match. */
#define KV_INSTANCES_PREFIX "kvscf:instances:"
#define KV_EDGE_PREFIX      "kvscf:edge:"
#define KV_APPS_PREFIX      "kvscf:apps:"
#define KV_LAUNCHER_PREFIX  "kvscf:launcher:"
#define KV_SCAN_COUNT 64
#define KV_MAX_HOSTS  8      /* distinct publisher hosts (only cleo today) */
/* Command payload: token + the largest possible id (a folder URI for a closed
 * Code favorite, not just an HWND) + JSON overhead. Sizing this off KV_ID_MAX
 * matters: too small and the payload builder returns 0, so the tap silently
 * does nothing. */
#define KV_PAYLOAD_MAX (KV_TOKEN_MAX + KV_ID_MAX + 64)
#define KV_KEY_MAX    96     /* "kvscf:instances:" + host */
#define KV_VALUE_MAX  32768  /* upper bound on one host's instance JSON */

static redis_client_t g_kv;
static bool           g_reachable;
static char           g_token[KV_TOKEN_MAX];
static char           g_pair[KV_HOST_MAX];

void kvscf_redis_init(const char *host, int port, const char *auth,
                      const char *token, const char *pair_host) {
    redis_client_init(&g_kv, host, port, auth);
    g_reachable = false;
    snprintf(g_token, sizeof(g_token), "%s", token ? token : "");
    kvscf_trim_trailing(g_token); /* byte-exact match — kill any CR/LF/space */

    snprintf(g_pair, sizeof(g_pair), "%s", pair_host ? pair_host : "");
    kvscf_trim_trailing(g_pair);
    /* Say out loud which workstation this panel will read and command. On a
     * shared server that is the whole of the scoping, so it is the line a
     * cut-over is verified by — and a setting that failed the host contract
     * has to be visible, because it degrades to the wildcard rather than
     * stopping the panel. */
    if (g_pair[0] != '\0' && !kvscf_pair_allows(g_pair, g_pair))
        fprintf(stderr,
                "kdeskdash: warning — KDESKDASH_KVSCF_PAIR_HOST \"%s\" is not a "
                "legal host; reading every publisher on %s:%d\n",
                g_pair, host ? host : "127.0.0.1", port);
    else
        printf("kdeskdash: kvscf pair %s (%s:%d)\n",
               g_pair[0] != '\0' ? g_pair : "<any publisher>",
               host ? host : "127.0.0.1", port);
}

void kvscf_redis_shutdown(void) {
    redis_client_close(&g_kv);
    g_reachable = false;
    /* Clear the secret from memory on teardown. */
    memset(g_token, 0, sizeof(g_token));
    memset(g_pair, 0, sizeof(g_pair));
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
    char match[KV_KEY_MAX];
    if (kvscf_scan_match(KV_INSTANCES_PREFIX, g_pair, match, sizeof match) == 0)
        return 0;
    int nkeys = scan_keys(match, keys, KV_MAX_HOSTS);
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
    char match[KV_KEY_MAX];
    if (kvscf_scan_match(KV_EDGE_PREFIX, g_pair, match, sizeof match) == 0)
        return 0;
    int nkeys = scan_keys(match, keys, KV_MAX_HOSTS);
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

int kvscf_redis_refresh_apps(kvscf_appitem_t *out, int max) {
    if (!out || max <= 0)
        return 0;
    if (!redis_client_ensure(&g_kv)) {
        g_reachable = false;
        return 0;
    }
    char keys[KV_MAX_HOSTS][KV_KEY_MAX];
    char match[KV_KEY_MAX];
    if (kvscf_scan_match(KV_APPS_PREFIX, g_pair, match, sizeof match) == 0)
        return 0;
    int nkeys = scan_keys(match, keys, KV_MAX_HOSTS);
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
            count = kvscf_parse_apps_append(g->str, (size_t)g->len, out, count, max);
        freeReplyObject(g);
    }
    kvscf_sort_apps(out, count);
    return count;
}

static int cmp_key(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

bool kvscf_redis_refresh_launcher(kvscf_launcher_t *out) {
    if (!out)
        return false;
    if (!redis_client_ensure(&g_kv)) {
        g_reachable = false;
        return false;
    }
    char keys[KV_MAX_HOSTS][KV_KEY_MAX];
    char match[KV_KEY_MAX];
    if (kvscf_scan_match(KV_LAUNCHER_PREFIX, g_pair, match, sizeof match) == 0)
        return false;
    int nkeys = scan_keys(match, keys, KV_MAX_HOSTS);
    if (nkeys <= 0)
        return false;
    /* One panel is paired with one publishing host, but SCAN order is not
     * stable, so sort: whichever host is chosen, it is the same one every
     * poll rather than flapping between two grids. */
    qsort(keys, (size_t)nkeys, KV_KEY_MAX, cmp_key);

    for (int i = 0; i < nkeys; i++) {
        redisReply *g = redisCommand(g_kv.ctx, "GET %s", keys[i]);
        if (!g) {
            g_reachable = false;
            return false;
        }
        bool ok = false;
        if (g->type == REDIS_REPLY_STRING && g->len > 0 &&
            (size_t)g->len <= KV_VALUE_MAX)
            ok = kvscf_parse_launcher(g->str, (size_t)g->len, out);
        freeReplyObject(g);
        if (ok)
            return true;
    }
    return false;
}

/* Publish a focus command with the given RESP-safe JSON `payload` to
 * kvscf:focus:<host>. Shared by the id-based focus and the key-based launch. */
static bool publish_focus(const char *host, const char *payload) {
    /* The write half of CD-8's pair scoping. The payload carries the pairing
     * token, so publishing to a host that is not this panel's pair hands the
     * token to a machine it does not belong to — on a shared server, reachable
     * by a record this panel merely happened to read. Unpaired (rpidash3, a
     * private instance) keeps the pre-fold behaviour. */
    if (!kvscf_pair_allows(g_pair, host))
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

bool kvscf_redis_launch(const char *host, const char *app_key) {
    if (g_token[0] == '\0')
        return false;
    if (!host || !telemetry_host_token_ok(host, strlen(host)))
        return false;
    char payload[KV_PAYLOAD_MAX];
    if (kvscf_launch_payload(g_token, app_key, payload, sizeof(payload)) == 0)
        return false;
    return publish_focus(host, payload);
}

bool kvscf_redis_press(const char *host, const char *button_key) {
    if (g_token[0] == '\0')
        return false;
    if (!host || !telemetry_host_token_ok(host, strlen(host)))
        return false;
    char payload[KV_PAYLOAD_MAX];
    if (kvscf_press_payload(g_token, button_key, payload, sizeof(payload)) == 0)
        return false;
    return publish_focus(host, payload);
}

bool kvscf_redis_focus(const char *host, const char *id, bool maximize) {
    /* R8: never send an unauthenticated command. */
    if (g_token[0] == '\0')
        return false;
    if (!host || !telemetry_host_token_ok(host, strlen(host)))
        return false;

    char payload[KV_PAYLOAD_MAX];
    if (kvscf_focus_payload(g_token, id, maximize, payload, sizeof(payload)) == 0)
        return false;
    return publish_focus(host, payload);
}
