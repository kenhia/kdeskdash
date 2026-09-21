/**
 * @file redis.c
 * The generic Redis connection handle. Synchronous, single-threaded, driven
 * from the main loop. Connection/reconnect shape mirrors kpidash/src/redis.c:
 * 250 ms connect timeout, 50 ms read timeout, optional AUTH, and a
 * reconnect-if-needed gate before each op. All failures are swallowed so the
 * dashboard keeps running when an endpoint is absent.
 *
 * Nothing here talks to a particular feed. Each feed is a thin reader on its
 * own handle (telemetry.c, kvscf_redis.c, service_pub.c, panel_store.c's
 * migration), so a stall or backoff on one never reaches another.
 *
 * This file also held the CONTROL client until sprint 039 — the singleton on
 * the board's own Redis that carried the panel's durable state and its command
 * keys. See redis.h for where both went.
 */
#include "redis.h"
#include "redis_internal.h" /* private redis_client layout + hiredis */

#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

/* A blocking connect to an unreachable host stalls the single-threaded UI loop
 * for up to the connect timeout. Keep that timeout short and only retry a dead
 * connection every RECONNECT_BACKOFF_S so a missing/remote Redis can't make the
 * panel janky. (Against the default loopback host a down Redis refuses
 * instantly, so this only matters for remote hosts.) */
#define CONNECT_TIMEOUT_MS  250
#define RECONNECT_BACKOFF_S 5

/* ---- Generic connection handle (one per endpoint) ---- */

/* Resolve `host` to a numeric IP string in `out`. Returns true on success.
 * getaddrinfo has no timeout knob, so this can block on a slow/dead resolver —
 * callers resolve ONCE and cache the result (see c->addr) so the per-op
 * reconnect path never pays this cost, keeping the single UI thread responsive.
 * A numeric host resolves immediately (no DNS traffic). */
static bool resolve_host(const char *host, char *out, size_t outlen) {
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;    /* v4 or v6 */
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
        return false;
    bool ok = getnameinfo(res->ai_addr, res->ai_addrlen, out, (socklen_t)outlen,
                          NULL, 0, NI_NUMERICHOST) == 0;
    freeaddrinfo(res);
    return ok;
}

/* Fill c->addr with the resolved IP if not already cached. Best-effort: on
 * failure c->addr stays empty and the caller falls back to connecting by name.
 * Once cached the IP is reused for the handle's lifetime (a restart re-resolves;
 * acceptable for this appliance's static homelab addressing). */
static void ensure_resolved(redis_client_t *c) {
    if (c->addr[0] == '\0')
        (void)resolve_host(c->host, c->addr, sizeof(c->addr));
}

void redis_client_init(redis_client_t *c, const char *host, int port,
                       const char *auth) {
    c->ctx = NULL;
    strncpy(c->host, host ? host : "127.0.0.1", sizeof(c->host) - 1);
    c->host[sizeof(c->host) - 1] = '\0';
    c->addr[0] = '\0';
    c->port = port > 0 ? port : 6379;
    c->auth[0] = '\0';
    if (auth) {
        strncpy(c->auth, auth, sizeof(c->auth) - 1);
        c->auth[sizeof(c->auth) - 1] = '\0';
    }
    c->next_attempt = 0;
    /* Resolve once here, at init (startup) — off the render loop — so later
     * reconnects connect straight to the cached IP without touching DNS. */
    ensure_resolved(c);
}

bool redis_client_connect(redis_client_t *c) {
    struct timeval tv = {0, CONNECT_TIMEOUT_MS * 1000};
    /* Prefer the cached numeric IP; only if init-time resolution failed (e.g.
     * the resolver was down at boot) do we retry a name lookup here — and even
     * then just once per backoff interval, not on every op. */
    ensure_resolved(c);
    const char *target = c->addr[0] ? c->addr : c->host;
    redisContext *ctx = redisConnectWithTimeout(target, c->port, tv);
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
