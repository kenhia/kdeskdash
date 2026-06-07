/**
 * @file dev_graph.h
 * One scrolling telemetry chart for a single host, in one of two flavours:
 *
 *   DEV_GRAPH_CPU_RAM  — CPU avg + top core on primary Y (0–100 %),
 *                        RAM used on a dynamic secondary Y (0–ram_total MB).
 *   DEV_GRAPH_GPU_VRAM — GPU compute on primary Y (0–100 %) drawn as a thick
 *                        triple-trace band, VRAM used on a dynamic secondary Y
 *                        (0–vram_total MB).
 *
 * kpidash packs all five series onto one chart with a single shared secondary
 * axis; splitting into two charts means each owns its own secondary-range state
 * (CPU/RAM scales to RAM, GPU/VRAM scales to VRAM, independently).
 */
#ifndef KDESKDASH_DEV_GRAPH_H
#define KDESKDASH_DEV_GRAPH_H

#include "dev_telemetry.h"
#include "lvgl.h"

typedef enum {
    DEV_GRAPH_CPU_RAM = 0,
    DEV_GRAPH_GPU_VRAM,
} dev_graph_kind_t;

/* Build a chart of the given kind under `parent`. Returns the container object
 * (its private state hangs off user_data). */
lv_obj_t *dev_graph_create(lv_obj_t *parent, dev_graph_kind_t kind);

/* Push one sample, advancing the chart and refreshing the header stats. The
 * relevant fields are read per kind; a CPU-only sample (has_gpu == false) feeds
 * a GPU/VRAM chart flat zeros. */
void dev_graph_update(lv_obj_t *graph, const dev_sample_t *s);

/* Set the host label shown above the chart (no-op if unchanged). */
void dev_graph_set_host(lv_obj_t *graph, const char *host);

/* Mark a data discontinuity at the current right edge: a vertical "start line"
 * is drawn that scrolls left with the data, flagging that the samples to its
 * left and right are not contiguous in time (e.g. after the mode was inactive
 * or a new host was just assigned). The marker scrolls off the left edge after
 * POINT_COUNT samples. */
void dev_graph_mark_gap(lv_obj_t *graph);

/* Free the chart's private state and delete the object tree. */
void dev_graph_destroy(lv_obj_t *graph);

#endif /* KDESKDASH_DEV_GRAPH_H */
