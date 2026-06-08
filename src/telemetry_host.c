/**
 * @file telemetry_host.c
 * Pure host-token contract (no Redis, no LVGL). Host-testable.
 */
#include "telemetry_host.h"

#include <string.h>

#include "dev_telemetry.h" /* DEV_HOST_MAX */

static bool valid_host_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

bool telemetry_host_token_ok(const char *host, size_t len) {
    if (!host || len == 0 || len > (size_t)(DEV_HOST_MAX - 1))
        return false;
    for (size_t i = 0; i < len; i++)
        if (!valid_host_char(host[i]))
            return false;
    return true;
}

bool telemetry_host_from_key(const char *key, size_t keylen, char *out, size_t outsz) {
    if (!key || !out || outsz == 0)
        return false;

    const size_t plen = sizeof(TELEMETRY_KEY_PREFIX) - 1;
    const size_t slen = sizeof(TELEMETRY_KEY_SUFFIX) - 1;

    /* Must fit prefix + at least one host char + suffix. */
    if (keylen <= plen + slen)
        return false;
    if (memcmp(key, TELEMETRY_KEY_PREFIX, plen) != 0)
        return false;
    if (memcmp(key + keylen - slen, TELEMETRY_KEY_SUFFIX, slen) != 0)
        return false;

    const char *host = key + plen;
    size_t hlen = keylen - plen - slen;
    if (!telemetry_host_token_ok(host, hlen) || hlen >= outsz)
        return false;

    memcpy(out, host, hlen);
    out[hlen] = '\0';
    return true;
}
