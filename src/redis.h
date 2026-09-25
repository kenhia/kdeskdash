/**
 * @file redis.h
 * The generic synchronous Redis connection handle every feed in this program
 * runs on: lazy connect, short timeouts, reconnect backoff, failures swallowed.
 * Mirrors the shape of the sibling kpidash project.
 *
 * Four units embed one by value — telemetry.c (kpidash host metrics),
 * kvscf_redis.c (the workstation pair), service_pub.c (the write-only service
 * card) and panel_store.c (the one-time state migration). The claude feed is
 * the exception and always has been: it is a libkdash kdash_conn_t owned by
 * its mode.
 *
 * Every function is safe to call when the endpoint is unreachable: connects
 * lazily, swallows failures, and no-ops while disconnected. Redis is an
 * optional dependency — the dashboard runs fully by touch without it.
 *
 * Until sprint 039 this header also carried a CONTROL client: a singleton on
 * the board's own loopback Redis, holding the panel's durable state
 * (`kdeskdash:golz:*`, `kdeskdash:calc:regs`, `kdeskdash:dev:*`,
 * `kdeskdash:active_mode`) and its command keys (`kdeskdash:gol:settings`,
 * `kdeskdash:golz:settings`, `kdeskdash:screenshot`). Durable state is a file
 * now (panel_state.h / panel_store.h) and commands come from central
 * (panel_feed.h), which is what lets the boards stop running a Redis server
 * each. The legacy key names survive in panel_store.c, where the one-time
 * migration reads them.
 */
#ifndef KDESKDASH_REDIS_H
#define KDESKDASH_REDIS_H

#include <stdbool.h>
#include <stddef.h>

/* Opaque synchronous Redis connection handle: owns a hiredis context plus the
 * endpoint/auth and a per-handle reconnect backoff deadline. Independent
 * endpoints (control + telemetry) each use their own handle so a stall or
 * backoff on one never affects the other. Single-threaded; no locking.
 *
 * The layout lives in redis_internal.h, private to the Redis/telemetry
 * implementation units (redis.c, telemetry.c, kvscf_redis.c), so consumers of
 * this header get no compile-time coupling to hiredis. Those units embed a
 * handle by value and so include redis_internal.h for the full definition. */
typedef struct redis_client redis_client_t;

/* Store endpoint/auth on the handle (host NULL -> loopback, port <= 0 -> 6379).
 * Does not connect; the caller chooses eager (redis_client_connect) or lazy
 * (redis_client_ensure on first use). `auth` may be NULL.
 * Resolves the host to a numeric IP once here (at init/startup) and caches it,
 * so later reconnects never touch DNS on the UI thread. */
void redis_client_init(redis_client_t *c, const char *host, int port,
                       const char *auth);

/* Attempt a connection now, ignoring backoff: 250 ms connect timeout, 50 ms
 * read timeout, optional AUTH. Returns true and sets c->ctx on success. Used for
 * a best-effort eager connect; does not arm the backoff on failure.
 * Connects to the IP cached at init (see redis_client_init), so hostname
 * resolution — which hiredis would otherwise run unbounded on every connect,
 * stalling the single UI thread against a slow/dead resolver — is paid once at
 * startup, not on the render path. If init-time resolution failed, this retries
 * a name lookup at most once per backoff interval and falls back to connecting
 * by name; pinning an IP or an /etc/hosts entry remains the fix for a resolver
 * that is dead at boot. */
bool redis_client_connect(redis_client_t *c);

/* Ensure a live connection, honoring backoff. True if connected. Frees an
 * errored context, and after a failed connect waits before the next attempt so
 * an unreachable endpoint can't stall the UI loop on every op. */
bool redis_client_ensure(redis_client_t *c);

/* Drop the connection and arm the reconnect backoff, as redis_client_ensure
 * does for a context hiredis has flagged. For a command whose reply was an
 * ERROR: the socket is fine, so hiredis flags nothing, and a caller that just
 * retried would write at main-loop rate. Reconnecting also re-runs AUTH, which
 * is the cure for a -NOAUTH after the server restarted. Idempotent. */
void redis_client_drop(redis_client_t *c);

/* Close and free the handle's context (idempotent). */
void redis_client_close(redis_client_t *c);

#endif /* KDESKDASH_REDIS_H */
