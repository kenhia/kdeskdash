/**
 * @file claude_redis.h
 * Read-only client for the claude-feed Redis (agent activity + usage limits
 * published by publisher/claude-pub.sh, instance on rpidash2:6380 — for the
 * dashboard a localhost read).
 *
 * Runs on its own redis_client_t handle, fully independent of the control and
 * telemetry endpoints: lazy connect, its own backoff. A down endpoint never
 * stalls boot or the other feeds — every failure is swallowed and reported as
 * "unavailable". All calls run on the single UI thread.
 */
#ifndef KDESKDASH_CLAUDE_REDIS_H
#define KDESKDASH_CLAUDE_REDIS_H

#include <stdbool.h>

#include "claude_feed.h"

typedef struct {
    char host[CF_HOST_MAX];
    char sid[CF_SID_MAX];
} claude_key_t;

typedef enum {
    CLAUDE_OK = 0,  /* fetched and parsed into *out                    */
    CLAUDE_ABSENT,  /* key missing/expired/malformed (out zeroed)      */
    CLAUDE_UNAVAIL, /* endpoint unreachable this attempt (out zeroed)  */
} claude_status_t;

/* Initialise the handle (lazy — no connection attempted). host NULL/empty ->
 * "127.0.0.1"; port <= 0 -> 6379; auth NULL -> no AUTH. */
void claude_redis_init(const char *host, int port, const char *auth);

/* Close the handle and clear state. Safe when never connected. */
void claude_redis_shutdown(void);

/* Endpoint responded on the most recent attempt (see telemetry_reachable). */
bool claude_redis_reachable(void);

/* Advance session-key discovery by one bounded SCAN batch
 * (MATCH claude:session:*). Keys failing the host/sid token contract are
 * skipped at this choke point. Returns true when a pass completed and the
 * current key list was refreshed. */
bool claude_redis_discover_step(void);

/* Copy the current discovered session keys into `out` (capacity `max`).
 * Returns the number written. */
int claude_redis_keys(claude_key_t *out, int max);

/* HGETALL one session hash and parse it via cf_session_from_fields (which
 * enforces the status/ts liveness contract). */
claude_status_t claude_redis_get_session(const claude_key_t *k, cf_session_t *out);

/* HGETALL claude:limits into *out (out->valid on CLAUDE_OK). */
claude_status_t claude_redis_get_limits(cf_limits_t *out);

/* LRANGE claude:recent, parsing each record; malformed entries are skipped.
 * Returns the number of records written to `out` (0 on empty or unreachable —
 * distinguish via claude_redis_reachable()). */
int claude_redis_get_recent(cf_recent_t *out, int max);

#endif /* KDESKDASH_CLAUDE_REDIS_H */
