/**
 * @file service_pub.c
 * kpidash service-card publisher on a dedicated handle. See service_pub.h.
 *
 * All the contract logic (key shape, payload, escaping, throttle) lives in the
 * pure service_card core and is host-tested; this file is the Redis glue.
 */
#include "service_pub.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "redis.h"
#include "redis_internal.h" /* embeds a redis_client_t by value + touches .ctx */
#include "service_card.h"

static redis_client_t g_card;
static char           g_key[SERVICE_CARD_KEY_MAX];
static char           g_host[SERVICE_CARD_HOST_MAX];
static char           g_text[96];
static bool           g_enabled;
static time_t         g_last;

void service_pub_init(const char *host, int port, const char *auth,
                      const char *name, const char *version) {
    redis_client_init(&g_card, host, port, auth);
    g_last = 0;
    g_enabled = false;
    g_key[0] = '\0';

    char raw[SERVICE_CARD_HOST_MAX];
    if (gethostname(raw, sizeof(raw)) != 0)
        raw[0] = '\0';
    raw[sizeof(raw) - 1] = '\0';
    if (!service_card_short_host(raw, g_host, sizeof(g_host)))
        snprintf(g_host, sizeof(g_host), "%s", SERVICE_CARD_HOST_NONE);

    const char *n = (name && name[0] != '\0') ? name : SERVICE_CARD_NAME;
    if (!service_card_key(n, g_host, g_key, sizeof(g_key))) {
        /* A bad name is the only way here, and the symptom on the board would
         * be no card at all — which is indistinguishable from "not deployed".
         * Say it once, at startup, where it is actually readable. */
        fprintf(stderr,
                "kdeskdash: service card disabled — cannot build a key from "
                "name \"%s\" host \"%s\" (allowed: A-Za-z0-9._-)\n",
                n, g_host);
        return;
    }
    snprintf(g_text, sizeof(g_text), "%s", version ? version : "");
    g_enabled = true;
}

void service_pub_tick(void) {
    if (!g_enabled)
        return;
    time_t now = time(NULL);
    if (!service_card_due(g_last, now, SERVICE_CARD_INTERVAL_S))
        return;
    /* Deliberately does NOT advance g_last on failure, so a reconnect publishes
     * as soon as it lands. The handle's own backoff keeps that cheap. */
    if (!redis_client_ensure(&g_card))
        return;

    char payload[SERVICE_CARD_PAYLOAD_MAX];
    if (!service_card_payload((double)now, "ok", g_text, g_host,
                              SERVICE_CARD_ICON_DASHBOARD, payload, sizeof(payload)))
        return;

    /* No TTL: freshness is the dashboard's job, computed from the payload's
     * `ts`. SETEX here would evict the card instead of reddening it. */
    redisReply *r = redisCommand(g_card.ctx, "SET %s %s", g_key, payload);
    if (r) {
        freeReplyObject(r);
        g_last = now;
    }
}

void service_pub_shutdown(void) {
    if (g_enabled && redis_client_ensure(&g_card)) {
        char payload[SERVICE_CARD_PAYLOAD_MAX];
        if (service_card_payload((double)time(NULL), "down", g_text, g_host,
                                 SERVICE_CARD_ICON_DASHBOARD, payload,
                                 sizeof(payload))) {
            redisReply *r = redisCommand(g_card.ctx, "SET %s %s", g_key, payload);
            if (r)
                freeReplyObject(r);
        }
    }
    redis_client_close(&g_card);
    g_enabled = false;
    g_last = 0;
}
