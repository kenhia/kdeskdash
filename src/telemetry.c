/**
 * @file telemetry.c
 * Read-only kpidash host-telemetry client on a dedicated redis_client_t handle.
 */
#include "telemetry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "redis.h"
#include "redis_internal.h" /* embeds a redis_client_t by value + touches .ctx */
#include "telemetry_host.h"

/* SCAN MATCH pattern: kpidash:client:*:dev_telemetry */
#define TELEMETRY_SCAN_MATCH TELEMETRY_KEY_PREFIX "*" TELEMETRY_KEY_SUFFIX
/* Keys hinted per SCAN batch (one batch per discover step). */
#define TELEMETRY_SCAN_COUNT 64
/* Reject pathologically large telemetry payloads (a sample is well under 1 KB). */
#define TELEMETRY_REPLY_MAX 8192

static redis_client_t g_tlm;

/* Discovery state. `building` accumulates across the SCAN batches of one pass;
 * on completion it is published into `current` so the UI never sees a partial
 * list mid-scan. */
static unsigned long long g_cursor;
static char g_building[TELEMETRY_HOSTS_MAX][DEV_HOST_MAX];
static int g_building_n;
static char g_current[TELEMETRY_HOSTS_MAX][DEV_HOST_MAX];
static int g_current_n;

/* Endpoint reachability as of the last discover/sample attempt. */
static bool g_reachable;

void telemetry_init(const char *host, int port, const char *auth) {
    redis_client_init(&g_tlm, host, port, auth);
    g_cursor = 0;
    g_building_n = 0;
    g_current_n = 0;
    g_reachable = false;
}

void telemetry_shutdown(void) {
    redis_client_close(&g_tlm);
    g_cursor = 0;
    g_building_n = 0;
    g_current_n = 0;
    g_reachable = false;
}

bool telemetry_reachable(void) {
    return g_reachable;
}

/* Append `host` to the building list if new and there is room. */
static void building_add(const char *host) {
    if (g_building_n >= TELEMETRY_HOSTS_MAX)
        return;
    for (int i = 0; i < g_building_n; i++)
        if (strcmp(g_building[i], host) == 0)
            return;
    snprintf(g_building[g_building_n], DEV_HOST_MAX, "%s", host);
    g_building_n++;
}

static void publish_building(void) {
    memcpy(g_current, g_building, sizeof(g_current));
    g_current_n = g_building_n;
    g_building_n = 0;
    g_cursor = 0;
}

bool telemetry_discover_step(void) {
    if (!redis_client_ensure(&g_tlm)) {
        g_reachable = false;
        /* Abandon any in-flight pass so a reconnect starts from a clean cursor. */
        g_cursor = 0;
        g_building_n = 0;
        return false;
    }

    char curbuf[32];
    snprintf(curbuf, sizeof(curbuf), "%llu", g_cursor);
    redisReply *r = redisCommand(g_tlm.ctx, "SCAN %s MATCH %s COUNT %d", curbuf,
                                 TELEMETRY_SCAN_MATCH, TELEMETRY_SCAN_COUNT);
    if (!r) {
        /* Connection error: the next ensure() will reconnect after backoff.
         * Reset the pass so the reconnect resumes from a clean cursor rather
         * than a stale one that could publish a partial host list. */
        g_reachable = false;
        g_cursor = 0;
        g_building_n = 0;
        return false;
    }
    g_reachable = true;

    /* Expect a 2-tuple: [next-cursor (string), keys (array)]. Anything else is
     * treated as an empty, completed pass. */
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
            char host[DEV_HOST_MAX];
            if (telemetry_host_from_key(k->str, (size_t)k->len, host, sizeof(host)))
                building_add(host);
        }
    }
    freeReplyObject(r);

    /* Pass completes on cursor wrap, or early if the UI cap is already full. */
    if (g_cursor == 0 || g_building_n >= TELEMETRY_HOSTS_MAX) {
        publish_building();
        return true;
    }
    return false;
}

int telemetry_hosts(char out[][DEV_HOST_MAX], int max) {
    int n = (g_current_n < max) ? g_current_n : max;
    for (int i = 0; i < n; i++)
        snprintf(out[i], DEV_HOST_MAX, "%s", g_current[i]);
    return n;
}

telemetry_status_t telemetry_get_sample(const char *host, dev_sample_t *out) {
    memset(out, 0, sizeof(*out));

    /* Choke point: never build a key from an untrusted/oversized host. */
    if (!host || !telemetry_host_token_ok(host, strlen(host)))
        return TELEMETRY_ABSENT;

    if (!redis_client_ensure(&g_tlm)) {
        g_reachable = false;
        return TELEMETRY_UNAVAIL;
    }

    char key[sizeof(TELEMETRY_KEY_PREFIX) + DEV_HOST_MAX + sizeof(TELEMETRY_KEY_SUFFIX)];
    snprintf(key, sizeof(key), TELEMETRY_KEY_PREFIX "%s" TELEMETRY_KEY_SUFFIX, host);

    redisReply *r = redisCommand(g_tlm.ctx, "GET %s", key);
    if (!r) {
        g_reachable = false;
        return TELEMETRY_UNAVAIL; /* connection error; reconnect next attempt */
    }
    g_reachable = true;

    telemetry_status_t status;
    if (r->type == REDIS_REPLY_STRING && r->len > 0 &&
        (size_t)r->len <= TELEMETRY_REPLY_MAX &&
        dev_telemetry_parse(r->str, (size_t)r->len, out)) {
        status = TELEMETRY_OK;
    } else {
        /* nil, empty, oversized, wrong type, or unparseable -> absent. */
        status = TELEMETRY_ABSENT;
    }
    freeReplyObject(r);
    return status;
}
