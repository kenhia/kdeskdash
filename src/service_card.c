/**
 * @file service_card.c
 * Pure builder for a kpidash service-status card. See service_card.h.
 *
 * Everything here is string work against a contract kdeskdash does not own, so
 * the rule throughout is: refuse rather than emit something malformed. A key
 * with the wrong segment count and a payload that fails to parse are both
 * *silently* dropped by the dashboard — no card, no error — so a half-built
 * value would be indistinguishable from the publisher never having run.
 */
#include "service_card.h"

#include <stdio.h>
#include <string.h>

/* The key's segment charset, chosen to exclude ':' — see service_card_key. */
static bool segment_ok(const char *s) {
    if (!s || s[0] == '\0')
        return false;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            continue;
        return false;
    }
    return true;
}

bool service_card_short_host(const char *hostname, char *out, size_t outsz) {
    /* Must at least hold the sentinel, or there is no useful answer to give. */
    if (!out || outsz < sizeof(SERVICE_CARD_HOST_NONE))
        return false;

    char label[SERVICE_CARD_HOST_MAX];
    label[0] = '\0';
    if (hostname && hostname[0] != '\0') {
        size_t n = 0;
        while (hostname[n] != '\0' && hostname[n] != '.' && n < sizeof(label) - 1)
            n++;
        /* A label that filled the buffer was truncated mid-name; treat it as
         * unusable rather than publishing under a clipped host. */
        if (hostname[n] == '\0' || hostname[n] == '.') {
            memcpy(label, hostname, n);
            label[n] = '\0';
        }
    }

    if (!segment_ok(label)) {
        memcpy(out, SERVICE_CARD_HOST_NONE, sizeof(SERVICE_CARD_HOST_NONE));
        return true;
    }
    size_t len = strlen(label);
    if (len >= outsz)
        return false; /* caller's buffer too small — leave it untouched */
    memcpy(out, label, len + 1);
    return true;
}

bool service_card_key(const char *name, const char *host, char *out, size_t outsz) {
    if (!out || outsz == 0)
        return false;
    /* Both segments are validated before a single byte is written: a ':' in
     * either would split the key into 5 segments and the SCAN pattern
     * `kpidash:services:*:*` would still match it, so the card would appear
     * under a silently wrong identity. */
    if (!segment_ok(name) || !segment_ok(host))
        return false;

    int n = snprintf(NULL, 0, SERVICE_CARD_KEY_PREFIX "%s:%s", name, host);
    if (n < 0 || (size_t)n >= outsz)
        return false;
    snprintf(out, outsz, SERVICE_CARD_KEY_PREFIX "%s:%s", name, host);
    return true;
}

/* kpidash's state vocabulary. An unrecognised state is refused here, because
 * the dashboard's own handling of one is to skip the payload entirely. */
static bool state_ok(const char *state) {
    static const char *const states[] = {"ok", "unhealthy", "maintenance",
                                         "down", "unknown"};
    if (!state)
        return false;
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++)
        if (strcmp(state, states[i]) == 0)
            return true;
    return false;
}

/* Escape `in` as a JSON string body into `out`. Truncates (at a character
 * boundary) rather than overflowing: `text` is a human status line, so a
 * clipped version is worth more than no card. */
static void json_escape(const char *in, char *out, size_t outsz) {
    size_t w = 0;
    if (outsz == 0)
        return;
    for (const char *p = in ? in : ""; *p; p++) {
        unsigned char c = (unsigned char)*p;
        char esc[8];
        size_t len;
        switch (c) {
        case '"':  memcpy(esc, "\\\"", 2); len = 2; break;
        case '\\': memcpy(esc, "\\\\", 2); len = 2; break;
        case '\n': memcpy(esc, "\\n", 2);  len = 2; break;
        case '\r': memcpy(esc, "\\r", 2);  len = 2; break;
        case '\t': memcpy(esc, "\\t", 2);  len = 2; break;
        case '\b': memcpy(esc, "\\b", 2);  len = 2; break;
        case '\f': memcpy(esc, "\\f", 2);  len = 2; break;
        default:
            if (c < 0x20) {
                len = (size_t)snprintf(esc, sizeof(esc), "\\u%04x", c);
            } else {
                esc[0] = (char)c;
                len = 1;
            }
            break;
        }
        if (w + len >= outsz)
            break;
        memcpy(out + w, esc, len);
        w += len;
    }
    out[w] = '\0';
}

bool service_card_payload(double ts, const char *state, const char *text,
                          const char *host, int icon, char *out, size_t outsz) {
    if (!out || outsz == 0 || !state_ok(state))
        return false;
    if (host && !segment_ok(host))
        return false;

    char esc[SERVICE_CARD_PAYLOAD_MAX];
    json_escape(text, esc, sizeof(esc));

    char hostf[SERVICE_CARD_HOST_MAX + 16];
    hostf[0] = '\0';
    if (host)
        snprintf(hostf, sizeof(hostf), ",\"host\":\"%s\"", host);

    char iconf[32];
    iconf[0] = '\0';
    if (icon >= 0)
        snprintf(iconf, sizeof(iconf), ",\"icon\":%d", icon);

    /* Build into a scratch buffer first so a payload that does not fit the
     * caller's buffer is refused whole, never written truncated. */
    char tmp[SERVICE_CARD_PAYLOAD_MAX * 2];
    int n = snprintf(tmp, sizeof(tmp),
                     "{\"ts\":%.3f,\"state\":\"%s\",\"text\":\"%s\"%s%s}",
                     ts, state, esc, hostf, iconf);
    if (n < 0 || (size_t)n >= sizeof(tmp) || (size_t)n >= outsz)
        return false;
    memcpy(out, tmp, (size_t)n + 1);
    return true;
}

bool service_card_due(time_t last, time_t now, int interval_s) {
    if (last <= 0 || interval_s <= 0)
        return true;
    if (now < last)
        return true; /* clock stepped backwards; don't wedge until it catches up */
    return (now - last) >= interval_s;
}
