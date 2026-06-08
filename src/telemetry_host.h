/**
 * @file telemetry_host.h
 * Pure host-token contract for telemetry discovery keys. No Redis, no LVGL.
 *
 * Discovery keys have the exact shape `kpidash:client:<host>:dev_telemetry`.
 * The `<host>` segment is the single trusted choke point: a key that fails this
 * contract is skipped entirely and its host never reaches a GET, key
 * construction, or a UI label.
 */
#ifndef KDESKDASH_TELEMETRY_HOST_H
#define KDESKDASH_TELEMETRY_HOST_H

#include <stdbool.h>
#include <stddef.h>

#define TELEMETRY_KEY_PREFIX "kpidash:client:"
#define TELEMETRY_KEY_SUFFIX ":dev_telemetry"

/* True if `host` (length `len`, not necessarily NUL-terminated) is a legal host
 * token: non-empty, <= DEV_HOST_MAX-1 chars, charset [A-Za-z0-9._-] only. */
bool telemetry_host_token_ok(const char *host, size_t len);

/**
 * Extract and validate the `<host>` from a discovery key of length `keylen`.
 *
 * The key must be anchored exactly: it must start with TELEMETRY_KEY_PREFIX and
 * end with TELEMETRY_KEY_SUFFIX, with a non-empty, contract-valid host between
 * them. On success copies the host into `out` (capacity `outsz`, always
 * NUL-terminated) and returns true. On any violation returns false and leaves
 * `out` untouched — never truncates-and-uses a malformed key.
 */
bool telemetry_host_from_key(const char *key, size_t keylen, char *out, size_t outsz);

#endif /* KDESKDASH_TELEMETRY_HOST_H */
