/**
 * @file dev_telemetry.c
 * Pure parser for kpidash dev_telemetry JSON. No Redis, no LVGL, host-testable.
 */
#include "dev_telemetry.h"

#include <math.h>
#include <string.h>

#include "cJSON.h"

/* Clamp a JSON number field to a percentage in [0,100]. A missing field, a
 * non-number, or a non-finite value (e.g. 1e400 -> inf) yields 0. */
static float clamp_pct(const cJSON *o) {
    if (!cJSON_IsNumber(o))
        return 0.0f;
    double v = o->valuedouble;
    if (!isfinite(v) || v < 0.0)
        return 0.0f;
    if (v > 100.0)
        return 100.0f;
    return (float)v;
}

/* Clamp a JSON number field to a non-negative MB count in [0, INT32_MAX].
 * Guards the double->integer conversion against undefined behaviour on
 * negative, non-finite, or out-of-range values. The ceiling is INT32_MAX (not
 * UINT32_MAX) so every value stays safe to cast to the int32_t the LVGL chart
 * axis uses downstream — a larger value would wrap negative and invert the
 * axis. Real RAM/VRAM totals are millions of MB at most, far below the cap. */
static uint32_t clamp_mb(const cJSON *o) {
    if (!cJSON_IsNumber(o))
        return 0;
    double v = o->valuedouble;
    if (!isfinite(v) || v < 0.0)
        return 0;
    if (v > (double)INT32_MAX)
        return (uint32_t)INT32_MAX;
    return (uint32_t)v;
}

bool dev_telemetry_parse(const char *json, size_t len, dev_sample_t *out) {
    memset(out, 0, sizeof(*out));
    if (!json || len == 0)
        return false;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root)
        return false;
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    /* Build into a local so a mid-parse abort can never leave *out partially
     * populated; only commit on success. */
    dev_sample_t s;
    memset(&s, 0, sizeof(s));

    s.cpu_pct = clamp_pct(cJSON_GetObjectItemCaseSensitive(root, "cpu_pct"));
    s.top_core_pct = clamp_pct(cJSON_GetObjectItemCaseSensitive(root, "top_core_pct"));
    s.ram_used_mb = clamp_mb(cJSON_GetObjectItemCaseSensitive(root, "ram_used_mb"));
    s.ram_total_mb = clamp_mb(cJSON_GetObjectItemCaseSensitive(root, "ram_total_mb"));

    /* "gpu" present and an object => GPU host; null or absent => CPU-only. */
    const cJSON *gpu = cJSON_GetObjectItemCaseSensitive(root, "gpu");
    if (cJSON_IsObject(gpu)) {
        s.has_gpu = true;
        s.gpu_compute_pct = clamp_pct(cJSON_GetObjectItemCaseSensitive(gpu, "compute_pct"));
        s.vram_used_mb = clamp_mb(cJSON_GetObjectItemCaseSensitive(gpu, "vram_used_mb"));
        s.vram_total_mb = clamp_mb(cJSON_GetObjectItemCaseSensitive(gpu, "vram_total_mb"));
    }

    /* Optional host. Pre-006 publishers omit it; leave empty so the caller can
     * fall back to the host parsed from the Redis key. */
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(root, "host");
    if (cJSON_IsString(host) && host->valuestring && host->valuestring[0]) {
        strncpy(s.host, host->valuestring, sizeof(s.host) - 1);
        s.host[sizeof(s.host) - 1] = '\0';
    }

    cJSON_Delete(root);
    *out = s;
    return true;
}
