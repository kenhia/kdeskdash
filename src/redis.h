/**
 * @file redis.h
 * Optional Redis client: remote active-mode control, last-mode persistence,
 * and Game of Life settings injection. Mirrors the connection/reconnect/poll
 * shape of the sibling kpidash project.
 *
 * Every function is safe to call when Redis is unreachable: connects lazily,
 * swallows failures, and no-ops while disconnected. Redis is an optional
 * dependency — the dashboard runs fully by touch without it.
 *
 * Schema:
 *   kdeskdash:active_mode   (string) currently active mode id
 *   kdeskdash:gol:settings  (hash)   one-shot Game of Life settings injection
 */
#ifndef KDESKDASH_REDIS_H
#define KDESKDASH_REDIS_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include <hiredis/hiredis.h>

#include "gol.h"

/* Generic synchronous Redis connection handle: owns a hiredis context plus the
 * endpoint/auth and a per-handle reconnect backoff deadline. Independent
 * endpoints (control + telemetry) each use their own handle so a stall or
 * backoff on one never affects the other. Single-threaded; no locking. */
typedef struct {
    redisContext *ctx;
    char host[128];
    int port;
    char auth[256];
    time_t next_attempt;
} redis_client_t;

/* Store endpoint/auth on the handle (host NULL -> loopback, port <= 0 -> 6379).
 * Does not connect; the caller chooses eager (redis_client_connect) or lazy
 * (redis_client_ensure on first use). `auth` may be NULL. */
void redis_client_init(redis_client_t *c, const char *host, int port,
                       const char *auth);

/* Attempt a connection now, ignoring backoff: 250 ms connect timeout, 50 ms
 * read timeout, optional AUTH. Returns true and sets c->ctx on success. Used for
 * a best-effort eager connect; does not arm the backoff on failure. */
bool redis_client_connect(redis_client_t *c);

/* Ensure a live connection, honoring backoff. True if connected. Frees an
 * errored context, and after a failed connect waits before the next attempt so
 * an unreachable endpoint can't stall the UI loop on every op. */
bool redis_client_ensure(redis_client_t *c);

/* Close and free the handle's context (idempotent). */
void redis_client_close(redis_client_t *c);

/* Store host/port/auth and attempt an initial connection. `auth` may be NULL.
 * Always succeeds from the caller's view; a failed connect simply leaves the
 * client disconnected until a later poll reconnects. */
void redis_init(const char *host, int port, const char *auth);

/* Close the connection and free the context (idempotent). */
void redis_shutdown(void);

/* One poll cycle: reconnect if needed, GET kdeskdash:active_mode, and if it
 * names a registered mode different from the active one, switch to it via the
 * shell. No-op when Redis is unreachable. */
void redis_poll(void);

/* Persist the active mode id (SET kdeskdash:active_mode). No-op when down. */
void redis_set_active_mode(const char *id);

/* Read kdeskdash:active_mode into `buf` (GET). Returns true if a non-empty
 * value was read. Used once at startup to restore the last active mode. */
bool redis_get_active_mode(char *buf, size_t buflen);

/* Read and CLEAR kdeskdash:gol:settings (HGETALL then DEL), overwriting only
 * the fields present in the hash on `cfg`. Absent fields are left untouched so
 * the caller's randomized defaults survive. Returns true if any field was
 * applied. No-op/false when Redis is down or the key is absent. */
bool redis_apply_gol_settings(gol_settings_t *cfg);

#endif /* KDESKDASH_REDIS_H */
