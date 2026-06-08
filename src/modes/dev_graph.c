/**
 * @file dev_graph.c
 * Per-host telemetry chart (CPU/RAM or GPU/VRAM). Adapted from kpidash's
 * single-chart dev_graph, split into two charts so each has an independent
 * secondary Y range. Only GPU compute uses the triple-trace thick-line trick
 * (LVGL 9 has no per-series line width); VRAM is a single line.
 */
#include "modes/dev_graph.h"

#include <stdio.h>
#include <string.h>

/* 300 points = 5 min at the 1 s telemetry interval. */
#define POINT_COUNT 300

/* Triple-trace offsets (axis units) so three same-colour lines fuse into a
 * band ~2.5x the single-line width. Primary Y is 0–100 %. */
#define GPU_PCT_OFFSET 1

/* Series colours. */
#define COLOR_GPU   lv_color_hex(0x40c057) /* GPU compute % (green) */
#define COLOR_CPU   lv_color_hex(0x4dabf7) /* CPU avg %     (blue)  */
#define COLOR_TOP   lv_color_hex(0xb197fc) /* CPU top core % (mauve) */
#define COLOR_VRAM  lv_color_hex(0xff922b) /* VRAM MB       (orange) */
#define COLOR_RAM   lv_color_hex(0xff6b6b) /* RAM MB        (red)    */
#define COLOR_BG    lv_color_hex(0x05070d)
#define COLOR_GRID  lv_color_hex(0x1b2433)
#define COLOR_HOST  lv_color_hex(0xcfe0f5)
#define COLOR_CAP   lv_color_hex(0x7c93b3)
#define COLOR_GAP   lv_color_hex(0xe6ffe6) /* gap/start marker (pale green) */

typedef struct {
    dev_graph_kind_t kind;
    lv_obj_t *chart;
    lv_obj_t *host_lbl;
    lv_obj_t *stat_lbl; /* compact current-values line */
    lv_obj_t *overlay;  /* centered status panel (hidden unless set) */
    lv_obj_t *overlay_lbl;

    /* CPU/RAM series. */
    lv_chart_series_t *cpu_ser;
    lv_chart_series_t *top_ser;
    lv_chart_series_t *ram_ser;

    /* GPU/VRAM series (gpu compute is triple-traced). */
    lv_chart_series_t *gpu_lo_ser;
    lv_chart_series_t *gpu_hi_ser;
    lv_chart_series_t *gpu_ser;
    lv_chart_series_t *vram_ser;

    uint32_t mb_max; /* current secondary-axis ceiling (RAM or VRAM total) */

    /* Vertical "gap" marker: point index (0..POINT_COUNT-1) where a data
     * discontinuity occurred, or -1 when inactive. Decrements as data shifts
     * left, scrolling the marker off the chart. */
    int32_t gap_idx;
} dev_graph_priv_t;

/* Draw the gap/start marker (a full-height vertical line) on top of the chart,
 * tracking the shifting data via priv->gap_idx. */
static void chart_draw_gap_cb(lv_event_t *e) {
    dev_graph_priv_t *priv = lv_event_get_user_data(e);
    if (!priv || priv->gap_idx < 0)
        return;
    lv_obj_t *chart = lv_event_get_target(e);

    lv_area_t a;
    lv_obj_get_content_coords(chart, &a);
    int32_t w = a.x2 - a.x1;
    int32_t x = a.x1 + (int32_t)((int64_t)w * priv->gap_idx / (POINT_COUNT - 1));

    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = COLOR_GAP;
    dsc.width = 2;
    dsc.opa = LV_OPA_70;
    dsc.p1.x = x;
    dsc.p1.y = a.y1;
    dsc.p2.x = x;
    dsc.p2.y = a.y2;
    lv_draw_line(layer, &dsc);
}

static lv_obj_t *make_chart(lv_obj_t *parent) {
    lv_obj_t *chart = lv_chart_create(parent);
    lv_obj_set_width(chart, LV_PCT(100));
    lv_obj_set_flex_grow(chart, 1);
    lv_obj_set_style_bg_color(chart, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_line_color(chart, COLOR_GRID, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);

    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_point_count(chart, POINT_COUNT);
    lv_chart_set_div_line_count(chart, 4, 0);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_range(chart, LV_CHART_AXIS_SECONDARY_Y, 0, 16384);
    return chart;
}

lv_obj_t *dev_graph_create(lv_obj_t *parent, dev_graph_kind_t kind) {
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_height(cont, LV_PCT(100));
    lv_obj_set_flex_grow(cont, 1);
    lv_obj_set_style_bg_color(cont, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cont, 1, 0);
    lv_obj_set_style_border_color(cont, COLOR_GRID, 0);
    lv_obj_set_style_radius(cont, 6, 0);
    lv_obj_set_style_pad_all(cont, 6, 0);
    lv_obj_set_style_pad_row(cont, 2, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    /* Compact header: host name + one current-values line. */
    lv_obj_t *host_lbl = lv_label_create(cont);
    lv_label_set_text(host_lbl, kind == DEV_GRAPH_CPU_RAM ? "CPU / RAM" : "GPU / VRAM");
    lv_obj_set_style_text_font(host_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(host_lbl, COLOR_HOST, 0);

    lv_obj_t *stat_lbl = lv_label_create(cont);
    lv_label_set_text(stat_lbl, "--");
    lv_obj_set_style_text_font(stat_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(stat_lbl, COLOR_CAP, 0);

    lv_obj_t *chart = make_chart(cont);

    /* Status overlay: a translucent rounded panel centered on the chart, hidden
     * until a non-live state sets it. A child of the chart so it floats above
     * the series without scrolling with the data. */
    lv_obj_t *overlay = lv_obj_create(chart);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(overlay, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_set_style_radius(overlay, 6, 0);
    lv_obj_set_style_border_width(overlay, 1, 0);
    lv_obj_set_style_border_color(overlay, COLOR_GRID, 0);
    lv_obj_set_style_pad_all(overlay, 8, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *overlay_lbl = lv_label_create(overlay);
    lv_obj_set_style_text_font(overlay_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(overlay_lbl, COLOR_HOST, 0);
    lv_obj_center(overlay_lbl);

    dev_graph_priv_t *priv = lv_malloc(sizeof(*priv));
    memset(priv, 0, sizeof(*priv));
    priv->kind = kind;
    priv->chart = chart;
    priv->host_lbl = host_lbl;
    priv->stat_lbl = stat_lbl;
    priv->overlay = overlay;
    priv->overlay_lbl = overlay_lbl;
    priv->mb_max = 16384;
    priv->gap_idx = -1;

    lv_obj_add_event_cb(chart, chart_draw_gap_cb, LV_EVENT_DRAW_POST_END, priv);

    /* Add flanking traces before the main series so the main line renders on
     * top of its band. */
    if (kind == DEV_GRAPH_CPU_RAM) {
        priv->cpu_ser = lv_chart_add_series(chart, COLOR_CPU, LV_CHART_AXIS_PRIMARY_Y);
        priv->top_ser = lv_chart_add_series(chart, COLOR_TOP, LV_CHART_AXIS_PRIMARY_Y);
        priv->ram_ser = lv_chart_add_series(chart, COLOR_RAM, LV_CHART_AXIS_SECONDARY_Y);
        lv_chart_set_all_value(chart, priv->cpu_ser, 0);
        lv_chart_set_all_value(chart, priv->top_ser, 0);
        lv_chart_set_all_value(chart, priv->ram_ser, 0);
    } else {
        priv->gpu_lo_ser = lv_chart_add_series(chart, COLOR_GPU, LV_CHART_AXIS_PRIMARY_Y);
        priv->gpu_hi_ser = lv_chart_add_series(chart, COLOR_GPU, LV_CHART_AXIS_PRIMARY_Y);
        priv->gpu_ser = lv_chart_add_series(chart, COLOR_GPU, LV_CHART_AXIS_PRIMARY_Y);
        priv->vram_ser = lv_chart_add_series(chart, COLOR_VRAM, LV_CHART_AXIS_SECONDARY_Y);
        lv_chart_set_all_value(chart, priv->gpu_lo_ser, 0);
        lv_chart_set_all_value(chart, priv->gpu_hi_ser, 0);
        lv_chart_set_all_value(chart, priv->gpu_ser, 0);
        lv_chart_set_all_value(chart, priv->vram_ser, 0);
    }

    lv_obj_set_user_data(cont, priv);
    return cont;
}

/* Round + clamp a percentage to the chart's 0–100 primary axis. */
static int32_t clamp_pct(float v) {
    int32_t r = (int32_t)(v + 0.5f);
    if (r < 0)
        r = 0;
    if (r > 100)
        r = 100;
    return r;
}

/* Adopt a new secondary-axis ceiling when the host's total changes. */
static void update_mb_range(dev_graph_priv_t *priv, uint32_t total_mb) {
    if (total_mb > 0 && total_mb != priv->mb_max) {
        priv->mb_max = total_mb;
        lv_chart_set_range(priv->chart, LV_CHART_AXIS_SECONDARY_Y, 0,
                           (int32_t)total_mb);
    }
}

static void update_cpu_ram(dev_graph_priv_t *priv, const dev_sample_t *s) {
    update_mb_range(priv, s->ram_total_mb);
    lv_chart_set_next_value(priv->chart, priv->cpu_ser, clamp_pct(s->cpu_pct));
    lv_chart_set_next_value(priv->chart, priv->top_ser, clamp_pct(s->top_core_pct));

    int32_t ram = (int32_t)s->ram_used_mb;
    if (ram > (int32_t)priv->mb_max)
        ram = (int32_t)priv->mb_max;
    lv_chart_set_next_value(priv->chart, priv->ram_ser, ram);

    char buf[64];
    snprintf(buf, sizeof(buf), "CPU %.0f%%  top %.0f%%  RAM %.1f/%.1f GB",
             (double)s->cpu_pct, (double)s->top_core_pct,
             s->ram_used_mb / 1024.0, s->ram_total_mb / 1024.0);
    lv_label_set_text(priv->stat_lbl, buf);
}

static void update_gpu_vram(dev_graph_priv_t *priv, const dev_sample_t *s) {
    if (!s->has_gpu) {
        /* CPU-only host: keep the chart scrolling flat (Unit 7 reshapes the
         * layout for CPU-only hosts; here we just avoid stale data). */
        lv_chart_set_next_value(priv->chart, priv->gpu_lo_ser, 0);
        lv_chart_set_next_value(priv->chart, priv->gpu_hi_ser, 0);
        lv_chart_set_next_value(priv->chart, priv->gpu_ser, 0);
        lv_chart_set_next_value(priv->chart, priv->vram_ser, 0);
        lv_label_set_text(priv->stat_lbl, "no GPU");
        return;
    }

    update_mb_range(priv, s->vram_total_mb);

    int32_t gpu = clamp_pct(s->gpu_compute_pct);
    int32_t hi = gpu + GPU_PCT_OFFSET;
    int32_t lo = gpu - GPU_PCT_OFFSET;
    if (hi > 100)
        hi = 100;
    if (lo < 0)
        lo = 0;
    lv_chart_set_next_value(priv->chart, priv->gpu_lo_ser, lo);
    lv_chart_set_next_value(priv->chart, priv->gpu_hi_ser, hi);
    lv_chart_set_next_value(priv->chart, priv->gpu_ser, gpu);

    int32_t vram = (int32_t)s->vram_used_mb;
    if (vram > (int32_t)priv->mb_max)
        vram = (int32_t)priv->mb_max;
    lv_chart_set_next_value(priv->chart, priv->vram_ser, vram);

    char buf[64];
    snprintf(buf, sizeof(buf), "GPU %.0f%%  VRAM %u/%u MB",
             (double)s->gpu_compute_pct, s->vram_used_mb, s->vram_total_mb);
    lv_label_set_text(priv->stat_lbl, buf);
}

void dev_graph_update(lv_obj_t *graph, const dev_sample_t *s) {
    if (!graph || !s)
        return;
    dev_graph_priv_t *priv = lv_obj_get_user_data(graph);
    if (!priv)
        return;

    if (priv->kind == DEV_GRAPH_CPU_RAM)
        update_cpu_ram(priv, s);
    else
        update_gpu_vram(priv, s);

    /* One logical time-step elapsed: shift the gap marker left with the data. */
    if (priv->gap_idx >= 0)
        priv->gap_idx--;

    lv_chart_refresh(priv->chart);
}

void dev_graph_set_host(lv_obj_t *graph, const char *host) {
    if (!graph || !host)
        return;
    dev_graph_priv_t *priv = lv_obj_get_user_data(graph);
    if (!priv || !priv->host_lbl)
        return;
    const char *cur = lv_label_get_text(priv->host_lbl);
    if (cur && strcmp(cur, host) == 0)
        return;
    lv_label_set_text(priv->host_lbl, host);
}

void dev_graph_set_status(lv_obj_t *graph, const char *msg) {
    if (!graph)
        return;
    dev_graph_priv_t *priv = lv_obj_get_user_data(graph);
    if (!priv || !priv->overlay)
        return;
    if (!msg || !msg[0]) {
        lv_obj_add_flag(priv->overlay, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const char *cur = lv_label_get_text(priv->overlay_lbl);
    if (!cur || strcmp(cur, msg) != 0)
        lv_label_set_text(priv->overlay_lbl, msg);
    lv_obj_clear_flag(priv->overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(priv->overlay, LV_ALIGN_CENTER, 0, 0);
}

void dev_graph_mark_gap(lv_obj_t *graph) {
    if (!graph)
        return;
    dev_graph_priv_t *priv = lv_obj_get_user_data(graph);
    if (!priv)
        return;
    priv->gap_idx = POINT_COUNT - 1; /* right edge; scrolls left with the data */
    lv_obj_invalidate(priv->chart);
}

void dev_graph_destroy(lv_obj_t *graph) {
    if (!graph)
        return;
    dev_graph_priv_t *priv = lv_obj_get_user_data(graph);
    if (priv)
        lv_free(priv);
    lv_obj_delete(graph);
}
