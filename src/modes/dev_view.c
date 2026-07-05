/**
 * @file dev_view.c
 * Pure dev-mode rendering decisions (see dev_view.h). No LVGL/Redis.
 */
#include "modes/dev_view.h"

dev_side_view_t dev_side_resolve(const dev_side_inputs_t *in, uint32_t stale_ms) {
    if (!in->telemetry_ok)
        return DEV_SIDE_UNAVAIL;
    if (!in->assigned)
        return DEV_SIDE_EMPTY;
    if (!in->seen_in_discovery)
        return DEV_SIDE_OFFLINE;
    /* Assigned, discovered, reachable — but no valid sample has ever arrived.
     * Without this the side would render as a flat-zero LIVE chart, making a
     * host that publishes malformed/no telemetry look healthy at 0%. */
    if (!in->ever_live)
        return DEV_SIDE_WAITING;
    if (in->ms_since_ok >= stale_ms)
        return DEV_SIDE_STALE;
    return DEV_SIDE_LIVE;
}

void dev_gpu_gate_init(dev_gpu_gate_t *g) {
    g->cpu_only = false;
    g->absent_run = 0;
}

bool dev_gpu_gate_update(dev_gpu_gate_t *g, bool has_gpu, int loss_n) {
    if (has_gpu) {
        g->absent_run = 0;
        g->cpu_only = false;
        return false;
    }
    if (g->absent_run < loss_n) /* saturate; avoids overflow on long runs */
        g->absent_run++;
    if (g->absent_run >= loss_n)
        g->cpu_only = true;
    return g->cpu_only;
}
