/**
 * @file service_pub_internal.h
 * The I/O seam the service-card publisher runs on (WI 2281): the handle's
 * connect-with-backoff gate, the one write, the free that matches it, the drop
 * that sends a failed handle into backoff, and the clock.
 *
 * It exists because the card's one claim with real consequences — a Redis it
 * only WRITES to going away never stalls the panel, and the card comes back by
 * itself — lives entirely on the failure path, and `just check` runs no Redis.
 * Modelled on kdashdata's src/kdash_feed_internal.h (its WI 2246), including
 * the rules that keep the cost where it belongs:
 *
 *   - **Nothing public.** service_pub.h is unchanged; only service_pub.c and
 *     its test include this.
 *   - **The default is the real thing**, installed at load with no
 *     initialisation call. `service_pub_set_io(NULL)` puts it back.
 *   - **A reply the seam produced is freed by the seam**, so a fake owns its
 *     own allocations instead of depending on hiredis' allocator matching the
 *     test's.
 *
 * What it does NOT cover, stated so nobody mistakes it for the whole proof:
 * how long a real connect or read blocks. Those bounds are redis.c's
 * (250 ms connect, 50 ms read, 5 s backoff) and are exercised live — the
 * sprint 044 record carries the rpidash2 pass with rpi53:6379 dropped.
 */
#ifndef KDESKDASH_SERVICE_PUB_INTERNAL_H
#define KDESKDASH_SERVICE_PUB_INTERNAL_H

#include <stdbool.h>
#include <time.h>

#include "redis_internal.h" /* redis_client_t layout, redisReply */

typedef struct service_pub_io {
    /* A live connection, honouring the handle's backoff. False while the
     * endpoint is unreachable or backing off — the publisher then does
     * nothing this tick. Real: redis_client_ensure(). */
    bool (*ensure)(redis_client_t *c);

    /* SET key payload. Returns the reply (freed with `free_reply`), or NULL
     * when the command did not complete. Real: redisCommand(). */
    redisReply *(*set)(redis_client_t *c, const char *key, const char *payload);

    /* Frees what `set` handed back. NULL-safe, like freeReplyObject. */
    void (*free_reply)(void *reply);

    /* Drop the connection and arm the handle's backoff, for a reply that came
     * back as an error: the socket is fine, so hiredis flags nothing, and
     * without this the next tick would write again at main-loop rate.
     * Real: redis_client_drop(). */
    void (*drop)(redis_client_t *c);

    /* Seconds since the epoch. Real: time(NULL). */
    time_t (*now)(void);
} service_pub_io_t;

/* The seam currently installed, and the real one. */
const service_pub_io_t *service_pub_io(void);
const service_pub_io_t *service_pub_io_real(void);

/* Swap the seam. NULL restores the real implementation. Test-only. */
void service_pub_set_io(const service_pub_io_t *io);

/* How many publishes have landed since init, and whether the last attempt
 * failed. Test-only observers; the panel never branches on them. */
int  service_pub_published_count(void);
bool service_pub_failing(void);

#endif /* KDESKDASH_SERVICE_PUB_INTERNAL_H */
