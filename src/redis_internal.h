/**
 * @file redis_internal.h
 * Private layout of the Redis connection handle, shared only by the Redis /
 * telemetry implementation units (redis.c, telemetry.c, claude_redis.c).
 *
 * This is the one place that includes hiredis. Public consumers include
 * redis.h, which forward-declares redis_client_t as an opaque type, so touching
 * a handle's fields (or hiredis at all) is confined to the units that own the
 * connection. Do NOT include this header from UI/mode translation units.
 */
#ifndef KDESKDASH_REDIS_INTERNAL_H
#define KDESKDASH_REDIS_INTERNAL_H

#include <time.h>

#include <hiredis/hiredis.h>

#include "redis.h"

/* Generic synchronous Redis connection handle: owns a hiredis context plus the
 * endpoint/auth and a per-handle reconnect backoff deadline. Independent
 * endpoints (control + telemetry) each use their own handle so a stall or
 * backoff on one never affects the other. Single-threaded; no locking. */
struct redis_client {
    redisContext *ctx;
    char host[128];
    char addr[64];  /* numeric IP resolved from host once, then reused so the
                     * per-op reconnect path never calls getaddrinfo on the UI
                     * thread; "" until first resolved */
    int port;
    char auth[256];
    time_t next_attempt;
};

#endif /* KDESKDASH_REDIS_INTERNAL_H */
