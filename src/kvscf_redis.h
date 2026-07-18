/**
 * @file kvscf_redis.h
 * kvscf window-feed client for the `foreground` mode. Reads `kvscf:instances:*`
 * and PUBLISHes `kvscf:focus:<host>` on the 6380 data instance (the same
 * instance the claude feed uses), on its **own** redis_client_t handle — lazy
 * connect, own backoff, failure-isolated, exactly like the other feeds. PUBLISH
 * is an ordinary command (no pub/sub socket); kdeskdash never SUBSCRIBEs.
 *
 * All calls run on the single UI thread.
 */
#ifndef KDESKDASH_KVSCF_REDIS_H
#define KDESKDASH_KVSCF_REDIS_H

#include <stdbool.h>

#include "kvscf_feed.h"

/* Initialise the handle (lazy — no connection). host NULL/empty -> "127.0.0.1";
 * port <= 0 -> 6380; auth NULL -> no AUTH. `token` is the shared secret used to
 * authenticate focus commands; it is copied and trailing whitespace/CR/LF is
 * trimmed (byte-exact match). NULL/empty token disables focusing. */
void kvscf_redis_init(const char *host, int port, const char *auth,
                      const char *token);

/* Close the handle and clear state. Safe when never connected. */
void kvscf_redis_shutdown(void);

/* Endpoint responded on the most recent attempt. */
bool kvscf_redis_reachable(void);

/* Whether a non-empty focus token is configured (after trimming). */
bool kvscf_redis_have_token(void);

/* Refresh the full window list: SCAN kvscf:instances:*, GET + parse each host,
 * merge and sort by label into `out` (capacity `max`). Returns the count
 * written (0 on empty or unreachable — distinguish via kvscf_redis_reachable). */
int kvscf_redis_refresh(kvscf_instance_t *out, int max);

/* Publish a focus command for `host` (the window's publisher host) and window
 * `id`. Returns true if a command was sent. No-op returning false when the
 * token is unset, host/id invalid, payload build fails, or the endpoint is
 * unreachable — never sends an unauthenticated command. */
bool kvscf_redis_focus(const char *host, const char *id, bool maximize);

#endif /* KDESKDASH_KVSCF_REDIS_H */
