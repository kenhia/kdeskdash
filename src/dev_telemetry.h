/**
 * @file dev_telemetry.h
 * Pure parser for kpidash dev_telemetry JSON payloads. No Redis, no LVGL.
 */
#ifndef KDESKDASH_DEV_TELEMETRY_H
#define KDESKDASH_DEV_TELEMETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Max host token length (incl. NUL). Matches the host-token contract used by
 * the telemetry client (anchored, non-empty, <= 63 chars). */
#define DEV_HOST_MAX 64

/* One decoded dev_telemetry sample. Percentages are clamped to [0,100];
 * memory figures (MB) are clamped to [0, UINT32_MAX]. */
typedef struct {
    char host[DEV_HOST_MAX]; /* source host; empty if the payload omitted it */
    float cpu_pct;           /* aggregate CPU load, 0-100 */
    float top_core_pct;      /* hottest single core, 0-100 */
    uint32_t ram_used_mb;
    uint32_t ram_total_mb;
    bool has_gpu;            /* false when "gpu" is null or absent */
    float gpu_compute_pct;   /* 0-100; meaningful only when has_gpu */
    uint32_t vram_used_mb;
    uint32_t vram_total_mb;
} dev_sample_t;

/**
 * Parse a kpidash dev_telemetry JSON payload
 * (key kpidash:client:<host>:dev_telemetry).
 *
 * `json` need not be NUL-terminated; `len` bounds the parse (embedded-NUL
 * safe). On success returns true and fills *out. On any failure (NULL/empty
 * input, malformed JSON, or a non-object root) returns false and leaves *out
 * fully zeroed.
 *
 * A payload that omits "host" still parses successfully; out->host is left
 * empty so the caller can substitute the host parsed from the Redis key.
 */
bool dev_telemetry_parse(const char *json, size_t len, dev_sample_t *out);

#endif /* KDESKDASH_DEV_TELEMETRY_H */
