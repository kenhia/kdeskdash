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
#include "service_pub_internal.h"

/* ---- the I/O seam (WI 2281; see service_pub_internal.h) ---- */

static redisReply *real_set(redis_client_t *c, const char *key, const char *payload) {
    return redisCommand(c->ctx, "SET %s %s", key, payload);
}

static time_t real_now(void) { return time(NULL); }

static const service_pub_io_t REAL_IO = {
    .ensure = redis_client_ensure,
    .set = real_set,
    .free_reply = freeReplyObject,
    .drop = redis_client_drop,
    .now = real_now,
};

static const service_pub_io_t *g_io = &REAL_IO;

const service_pub_io_t *service_pub_io(void) { return g_io; }
const service_pub_io_t *service_pub_io_real(void) { return &REAL_IO; }
void service_pub_set_io(const service_pub_io_t *io) { g_io = io ? io : &REAL_IO; }

static redis_client_t g_card;
static char           g_key[SERVICE_CARD_KEY_MAX];
static char           g_host[SERVICE_CARD_HOST_MAX];
static char           g_text[96];
static bool           g_enabled;
static time_t         g_last;
static int            g_published;
static bool           g_failing;

int service_pub_published_count(void) { return g_published; }
bool service_pub_failing(void) { return g_failing; }

void service_pub_init(const char *host, int port, const char *auth,
                      const char *name, const char *version) {
    redis_client_init(&g_card, host, port, auth);
    g_last = 0;
    g_enabled = false;
    g_published = 0;
    g_failing = false;
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
    time_t now = g_io->now();
    if (!service_card_due(g_last, now, SERVICE_CARD_INTERVAL_S))
        return;
    /* Deliberately does NOT advance g_last on failure, so a reconnect publishes
     * as soon as it lands. The handle's own backoff keeps that cheap — for a
     * NULL reply hiredis arms it, for an error reply we do (below).
     * test_service_pub drives both through the seam. */
    if (!g_io->ensure(&g_card))
        return;

    char payload[SERVICE_CARD_PAYLOAD_MAX];
    if (!service_card_payload((double)now, "ok", g_text, g_host,
                              SERVICE_CARD_ICON_DASHBOARD, payload, sizeof(payload)))
        return;

    /* No TTL: freshness is the dashboard's job, computed from the payload's
     * `ts`. SETEX here would evict the card instead of reddening it. */
    redisReply *r = g_io->set(&g_card, g_key, payload);
    bool ok = r && r->type != REDIS_REPLY_ERROR;
    char why[96];
    snprintf(why, sizeof(why), "%s",
             !r ? "no reply" : (r->str && !ok ? r->str : "error reply"));
    if (r && !ok) {
        /* The server answered, and the answer was no (-MISCONF when rpi53's
         * disk fills, -READONLY, -NOAUTH). hiredis flags nothing — the socket
         * is fine — so drop the handle into backoff ourselves: reconnecting
         * re-runs AUTH, and retrying without it would write every tick.
         * Before WI 2281 this counted as a publish, which is exactly what the
         * seam's first run caught. */
        g_io->drop(&g_card);
    }
    g_io->free_reply(r);
    if (ok) {
        g_last = now;
        g_published++;
        if (g_failing)
            fprintf(stderr, "kdeskdash: service card publishing again\n");
        g_failing = false;
    } else if (!g_failing) {
        /* Once per outage, not per attempt: the journal gets the transition,
         * the panel gets nothing at all. A NULL reply has already flagged the
         * context, and the handle's own backoff takes it from here. */
        fprintf(stderr, "kdeskdash: service card publish failed (%s); "
                        "retrying on the handle's backoff\n", why);
        g_failing = true;
    }
}

void service_pub_shutdown(void) {
    if (g_enabled && g_io->ensure(&g_card)) {
        char payload[SERVICE_CARD_PAYLOAD_MAX];
        if (service_card_payload((double)g_io->now(), "down", g_text, g_host,
                                 SERVICE_CARD_ICON_DASHBOARD, payload,
                                 sizeof(payload))) {
            redisReply *r = g_io->set(&g_card, g_key, payload);
            g_io->free_reply(r);
        }
    }
    redis_client_close(&g_card);
    g_enabled = false;
    g_last = 0;
}
