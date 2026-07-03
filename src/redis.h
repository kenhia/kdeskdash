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
 *   kdeskdash:active_mode    (string) currently active mode id
 *   kdeskdash:gol:settings   (hash)   one-shot Game of Life settings injection
 *   kdeskdash:golz:wins         (string) historical zombie-win counter (legacy,
 *                                        pre-machetes; displayed read-only)
 *   kdeskdash:golz:human_wins   (string) post-machete Human win counter
 *   kdeskdash:golz:zombie_wins  (string) post-machete Zombie win counter
 *   kdeskdash:golz:ties         (string) post-machete Tie counter
 *   kdeskdash:golz:gens_to_win  (string) adaptive Human-win generation threshold
 *   kdeskdash:golz:settings     (hash)   one-shot GoLZ settings injection
 *   kdeskdash:screenshot        (string) one-shot device self-screenshot trigger,
 *                                        consumed with GETDEL; a value starting
 *                                        with '/' names the BMP output path
 */
#ifndef KDESKDASH_REDIS_H
#define KDESKDASH_REDIS_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include <hiredis/hiredis.h>

#include "golz.h"

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
 * a best-effort eager connect; does not arm the backoff on failure.
 * NOTE: the 250 ms timeout bounds the TCP connect but NOT hostname resolution
 * (hiredis calls getaddrinfo() unbounded). For a remote endpoint configured by
 * name, a slow/dead resolver can still stall the UI thread — prefer an IP or an
 * /etc/hosts pin. A future async/at-init resolve is tracked as follow-up work. */
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

/* Atomically increment and return the persistent GoLZ win counter
 * (INCR kdeskdash:golz:wins). Returns the post-increment count, or -1 when
 * Redis is unreachable or the reply is unusable (caller treats <0 as unknown). */
long redis_golz_incr_wins(void);

/* Read the historical (legacy, pre-machetes) GoLZ zombie-win counter
 * (GET kdeskdash:golz:wins), returning `default_val` when missing, unparseable,
 * or negative. Displayed read-only; the post-machete era uses the counters below.
 * No-op-safe when down. */
long redis_golz_get_wins(long default_val);

/* Atomically increment and return a post-machete outcome counter
 * (Human / Zombie / Tie). Returns the post-increment count, or -1 when Redis is
 * unreachable or the reply is unusable (caller treats <0 as unknown). */
long redis_golz_incr_human_wins(void);
long redis_golz_incr_zombie_wins(void);
long redis_golz_incr_ties(void);

/* Read a post-machete outcome counter, returning `default_val` when missing,
 * unparseable, or negative. No-op-safe when down. */
long redis_golz_get_human_wins(long default_val);
long redis_golz_get_zombie_wins(long default_val);
long redis_golz_get_ties(long default_val);

/* Get/set the adaptive Human-win generation threshold
 * (kdeskdash:golz:gens_to_win). get returns `default_val` when missing; both
 * enforce the game-rule floor of 100. set returns the value actually stored
 * (still returns the floored value when Redis is down so the caller can mirror
 * it in memory). */
long redis_golz_get_gens_to_win(long default_val);
long redis_golz_set_gens_to_win(long value);

/* Read and CLEAR kdeskdash:golz:settings (HGETALL then DEL), overwriting only
 * the fields present in the hash on `cfg`. Absent fields are left untouched so
 * the caller's randomized defaults survive. Returns true if any field was
 * applied. No-op/false when Redis is down or the key is absent. */
bool redis_apply_golz_settings(golz_settings_t *cfg);

/* Which dev-mode chart side a host assignment belongs to. */
typedef enum {
    REDIS_DEV_SIDE_LEFT = 0,
    REDIS_DEV_SIDE_RIGHT,
} redis_dev_side_t;

/* Persist a dev-mode host assignment (SET kdeskdash:dev:left|right). An empty
 * or NULL host clears the slot (DEL). No-op when the control Redis is down. */
void redis_set_dev_assignment(redis_dev_side_t side, const char *host);

/* Read a persisted dev-mode host assignment into `buf` (GET). Returns true if a
 * non-empty value was read. The value is untrusted — the caller must re-validate
 * it against the host-token contract before using it to build a telemetry key. */
bool redis_get_dev_assignment(redis_dev_side_t side, char *buf, size_t buflen);

#endif /* KDESKDASH_REDIS_H */
