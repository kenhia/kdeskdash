/**
 * @file service_pub.h
 * Publishes this instance's kpidash service-status card. Write-only.
 *
 * Runs on its own redis_client_t handle with its own endpoint config, like
 * every other feed here: a dead or slow kpidash Redis never stalls boot, the
 * render loop, or another path. kdeskdash does NOT read the
 * `kpidash:services:*:*` namespace — the card is rendered by the kpidash
 * dashboard on rpi53.
 *
 * Unlike the telemetry/claude/kvscf feeds this is not gated on a mode: the card
 * says "this instance is alive", which is true whichever modes a panel
 * registers, so it is initialised unconditionally.
 */
#ifndef KDESKDASH_SERVICE_PUB_H
#define KDESKDASH_SERVICE_PUB_H

/* Initialise the handle and build this instance's key (lazy — no connection is
 * attempted here). `name` NULL/empty -> SERVICE_CARD_NAME. `version` is what
 * the card's text line shows. The host segment comes from gethostname(); if the
 * key cannot be built the publisher disables itself and says so once. */
void service_pub_init(const char *host, int port, const char *auth,
                      const char *name, const char *version);

/* Publish the card if the interval has elapsed. Cheap and safe to call every
 * main-loop iteration: throttled to SERVICE_CARD_INTERVAL_S, and a no-op while
 * the endpoint is unreachable (the handle's own backoff applies). */
void service_pub_tick(void);

/* Best-effort `down` publish, then close the handle (idempotent). The card is
 * deliberately left on the board — kpidash has no TTL on it, and a dashboard
 * that stopped is worth seeing. */
void service_pub_shutdown(void);

#endif /* KDESKDASH_SERVICE_PUB_H */
