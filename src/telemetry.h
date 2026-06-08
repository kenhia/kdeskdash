/**
 * @file telemetry.h
 * Read-only client for kpidash host telemetry, published to a (typically
 * remote) Redis under keys `kpidash:client:<host>:dev_telemetry`.
 *
 * Runs on its own redis_client_t handle, fully independent of the local control
 * Redis: lazy connect, its own backoff. A down or slow telemetry endpoint never
 * stalls boot and never blocks the control path — every failure is swallowed and
 * reported as "unavailable".
 *
 * All calls run on the single UI thread (no internal threads/locks).
 */
#ifndef KDESKDASH_TELEMETRY_H
#define KDESKDASH_TELEMETRY_H

#include <stdbool.h>
#include <stddef.h>

#include "dev_telemetry.h" /* dev_sample_t, DEV_HOST_MAX */

/* Hard cap on hosts surfaced to the UI (desk view shows a small set). */
#define TELEMETRY_HOSTS_MAX 16

typedef enum {
    TELEMETRY_OK = 0,   /* key present and parsed into *out                  */
    TELEMETRY_ABSENT,   /* key missing/expired/empty/malformed (out zeroed)  */
    TELEMETRY_UNAVAIL,  /* endpoint unreachable this attempt (out zeroed)    */
} telemetry_status_t;

/* Initialise the telemetry handle (lazy — no connection is attempted here).
 * host NULL/empty -> "127.0.0.1"; port <= 0 -> 6379; auth NULL -> no AUTH.
 * Resets the discovery cursor and host list. */
void telemetry_init(const char *host, int port, const char *auth);

/* Close the handle and clear state. Safe to call when never connected. */
void telemetry_shutdown(void);

/* Advance host discovery by one bounded SCAN batch (resuming the cursor across
 * calls). When a full scan pass completes the freshly-found host set is
 * published atomically as the current list. Returns true when a pass completed
 * on this call (the list was just refreshed), false otherwise (mid-pass, or the
 * endpoint was unreachable). Cheap and non-blocking beyond one socket round-trip
 * under the handle's read timeout. */
bool telemetry_discover_step(void);

/* Copy the current known host list into `out` (capacity `max` rows of
 * DEV_HOST_MAX). Returns the number of hosts written. */
int telemetry_hosts(char out[][DEV_HOST_MAX], int max);

/* Whether the telemetry endpoint responded on the most recent discover/sample
 * attempt. False before the first successful round-trip and whenever the last
 * attempt hit a connect/read error. Lets the UI show a single app-wide
 * "telemetry unavailable" state instead of a misleading empty selector. */
bool telemetry_reachable(void);

/* Fetch and parse one host's latest sample. `host` is re-validated against the
 * token contract before any key is built. On TELEMETRY_OK, *out holds the
 * sample; otherwise *out is zeroed. Never blocks beyond one round-trip. */
telemetry_status_t telemetry_get_sample(const char *host, dev_sample_t *out);

#endif /* KDESKDASH_TELEMETRY_H */
