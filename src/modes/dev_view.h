/**
 * @file dev_view.h
 * Pure, host-testable decision logic for dev-mode per-side rendering (no LVGL,
 * no Redis): the liveness precedence ladder and the CPU-only layout debounce.
 *
 * Kept separate from dev.c so the state machine can be unit-tested without a
 * display or a telemetry endpoint.
 */
#ifndef KDESKDASH_DEV_VIEW_H
#define KDESKDASH_DEV_VIEW_H

#include <stdbool.h>
#include <stdint.h>

/* One side renders exactly one of these states; higher entries win when several
 * conditions co-occur (UNAVAIL supersedes everything, then EMPTY, ...). */
typedef enum {
    DEV_SIDE_UNAVAIL = 0, /* telemetry endpoint unreachable (global, R4)      */
    DEV_SIDE_EMPTY,       /* no host assigned to this side (R17)              */
    DEV_SIDE_OFFLINE,     /* assigned host absent from discovery (R18)        */
    DEV_SIDE_STALE,       /* was live, samples stopped >= stale_ms (R16)      */
    DEV_SIDE_LIVE,        /* streaming fresh samples                          */
} dev_side_view_t;

typedef struct {
    bool     telemetry_ok;      /* endpoint reachable on the last attempt     */
    bool     assigned;          /* a host is assigned to this side            */
    bool     seen_in_discovery; /* assigned host is online in discovery       */
    bool     ever_live;         /* >= 1 OK sample since assignment            */
    uint32_t ms_since_ok;       /* elapsed since last OK sample (if ever_live) */
} dev_side_inputs_t;

/* Resolve the single state to render for a side. `stale_ms` is the freshness
 * threshold (e.g. 10000). */
dev_side_view_t dev_side_resolve(const dev_side_inputs_t *in, uint32_t stale_ms);

/* CPU-only layout debounce. A host that reports `has_gpu == false` should
 * collapse to a single centered CPU/RAM chart, but only after a short run of
 * consecutive GPU-absent samples so a one-off idle blip doesn't flap the
 * layout. A single GPU-present sample restores the two-chart layout at once. */
typedef struct {
    bool cpu_only;   /* current layout decision (true == centered single chart) */
    int  absent_run; /* consecutive has_gpu==false samples                      */
} dev_gpu_gate_t;

void dev_gpu_gate_init(dev_gpu_gate_t *g);

/* Feed one sample's has_gpu. Returns the (possibly unchanged) cpu_only decision.
 * `loss_n` is the consecutive-absent count required to switch into CPU-only. */
bool dev_gpu_gate_update(dev_gpu_gate_t *g, bool has_gpu, int loss_n);

#endif /* KDESKDASH_DEV_VIEW_H */
