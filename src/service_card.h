/**
 * @file service_card.h
 * Pure builder for a kpidash service-status card. No Redis, no LVGL.
 *
 * kpidash's dashboard on rpi53 renders a card for every key matching
 * `kpidash:services:*:*`. Publishing one is the whole contract — kdeskdash
 * never reads that namespace. The wire format is kpidash's, not ours:
 *
 *   key    kpidash:services:<name>:<host>   exactly 4 colon-separated segments
 *   value  STRING holding compact JSON, no TTL
 *
 * Freshness is computed dashboard-side from the payload's `ts`: any state other
 * than `down`/`unknown` renders its own colour only while `now - ts < 60 s`,
 * and RED after that. So the publish interval must stay well under 60 s —
 * kpidash's own self-card uses 15 s and SERVICE_CARD_INTERVAL_S mirrors it.
 *
 * A three-segment key is silently ignored by the dashboard (no card, no error),
 * which is why `name` and `host` are validated here and a key is never built
 * from a segment containing ':'.
 */
#ifndef KDESKDASH_SERVICE_CARD_H
#define KDESKDASH_SERVICE_CARD_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define SERVICE_CARD_KEY_PREFIX "kpidash:services:"

/* The name segment this project publishes under. */
#define SERVICE_CARD_NAME "deskdash"

/* Sentinel host segment for a publisher that is not host-scoped. kdeskdash is
 * host-scoped, so this is only ever a fallback for an unreadable hostname. */
#define SERVICE_CARD_HOST_NONE "_"

/* kpidash icon registry index 20 — monitor_dashboard, the glyph kpidash's own
 * self-card uses. The registry is append-only; indices are never reused. An
 * unregistered index renders no icon rather than breaking the card. */
#define SERVICE_CARD_ICON_DASHBOARD 20

/* Publish cadence, comfortably under the dashboard's 60 s staleness cutoff. */
#define SERVICE_CARD_INTERVAL_S 15

#define SERVICE_CARD_HOST_MAX    64
#define SERVICE_CARD_KEY_MAX     160
#define SERVICE_CARD_PAYLOAD_MAX 320

/* Shorten a hostname to its first label ("kai.local" -> "kai"), validate it
 * against the segment charset [A-Za-z0-9._-] (minus ':'), and **lowercase** it
 * ("rpiDash2" -> "rpidash2") so the card key matches every other fleet
 * reference. Mixed case is valid input, not an error — it is normalised, not
 * rejected. NULL, empty, or an unusable hostname yields SERVICE_CARD_HOST_NONE
 * rather than failing, so a broken gethostname() still produces a card. Returns
 * false only when `out` is too small (in which case `out` is left untouched). */
bool service_card_short_host(const char *hostname, char *out, size_t outsz);

/* Build `kpidash:services:<name>:<host>`. Both segments must be non-empty and
 * free of ':'; anything else returns false and leaves `out` untouched, because
 * a malformed key is invisible on the board rather than loud. */
bool service_card_key(const char *name, const char *host, char *out, size_t outsz);

/* Build the compact JSON payload. `ts` is unix seconds (fractional). `state` is
 * one of ok/unhealthy/maintenance/down/unknown — an unrecognised state returns
 * false rather than publishing a payload the dashboard will skip. `text` is the
 * short status line and is JSON-escaped. `host` may be NULL to omit the field;
 * `icon` < 0 omits the icon. Returns false (leaving `out` untouched) on bad
 * input or if the result would not fit. */
bool service_card_payload(double ts, const char *state, const char *text,
                          const char *host, int icon, char *out, size_t outsz);

/* True when a publish is due: never published (last == 0), the interval has
 * elapsed, or the clock moved backwards (re-publish rather than stall until it
 * catches up). */
bool service_card_due(time_t last, time_t now, int interval_s);

#endif /* KDESKDASH_SERVICE_CARD_H */
