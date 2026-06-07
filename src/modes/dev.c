/**
 * @file dev.c
 * Dev mode: mirrored four-chart host-telemetry view. Two hosts (left/right)
 * each drive a CPU/RAM and a GPU/VRAM chart, laid out
 * `[CPU/RAM][GPU/VRAM]  selector  [GPU/VRAM][CPU/RAM]` so GPU/VRAM is inner on
 * both sides. Telemetry is polled (~1 s) only while the mode is active.
 *
 * The center column is a live host selector: tap a host row to select it, then
 * tap the left/right assign control to place it on that side. Rows show L/R
 * markers for current assignments and dim when a host goes offline. The host
 * list itself (sort, debounce, keep-selected) lives in the pure dev_hostlist
 * model; this file owns the LVGL rendering and telemetry I/O.
 *
 * Assignments are not yet persisted — Unit 6 adds restore-on-activate; Unit 7
 * adds richer liveness/CPU-only handling.
 */
#include "modes/dev.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "modes/dev_graph.h"
#include "modes/dev_hostlist.h"
#include "telemetry.h"

#define DEV_POLL_MS     1000 /* per-host GET cadence */
#define DEV_DISCOVER_MS 5000 /* SCAN discovery cadence (slower than poll) */
#define CENTER_W        260  /* center selector column width (px) */
#define ROW_H           40

#define COLOR_BG       lv_color_hex(0x05070d)
#define COLOR_HOST     lv_color_hex(0xcfe0f5)
#define COLOR_ROW      lv_color_hex(0x2b3340)
#define COLOR_ROW_SEL  lv_color_hex(0x3d6fb0)
#define COLOR_TXT      lv_color_hex(0xeaf0fb)
#define COLOR_TXT_OFF  lv_color_hex(0x6b7280)
#define COLOR_ASSIGN   lv_color_hex(0x394150)

typedef struct {
    lv_obj_t *cpu;
    lv_obj_t *gpu;
    char host[DEV_HOST_MAX];
} dev_side_t;

typedef struct dev_state dev_state_t;

/* Per-row callback context (stable storage in dev_state, indexed by row). */
typedef struct {
    dev_state_t *st;
    char host[DEV_HOST_MAX];
} row_ctx_t;

struct dev_state {
    dev_side_t left;
    dev_side_t right;

    lv_obj_t *list;                       /* scrollable host-row container */
    lv_obj_t *rows[DEV_HOSTLIST_MAX];     /* row buttons (NULL when unused) */
    lv_obj_t *row_lbls[DEV_HOSTLIST_MAX]; /* row labels */
    row_ctx_t row_ctx[DEV_HOSTLIST_MAX];
    int       row_count;

    char selected[DEV_HOST_MAX]; /* "" == nothing selected */
    dev_hostlist_t hosts;

    uint32_t last_poll;
    uint32_t last_discover;
};

static bool host_eq(const char *a, const char *b) {
    return a[0] != '\0' && strcmp(a, b) == 0;
}

/* Repaint every row's label (markers + offline dim) and selection highlight,
 * without rebuilding widgets. */
static void repaint_rows(dev_state_t *st) {
    for (int i = 0; i < st->row_count; i++) {
        const char *host = st->row_ctx[i].host;
        bool isL = host_eq(st->left.host, host);
        bool isR = host_eq(st->right.host, host);
        const char *mark = isL && isR ? "LR " : isL ? "L  " : isR ? "R  " : "   ";

        char buf[DEV_HOST_MAX + 8];
        snprintf(buf, sizeof(buf), "%s%s", mark, host);
        lv_label_set_text(st->row_lbls[i], buf);

        bool online = true;
        for (int j = 0; j < st->hosts.count; j++)
            if (strcmp(st->hosts.entries[j].host, host) == 0) {
                online = st->hosts.entries[j].online;
                break;
            }
        lv_obj_set_style_text_color(st->row_lbls[i],
                                    online ? COLOR_TXT : COLOR_TXT_OFF, 0);

        bool selected = host_eq(st->selected, host);
        lv_obj_set_style_bg_color(st->rows[i],
                                  selected ? COLOR_ROW_SEL : COLOR_ROW, 0);
    }
}

static void row_cb(lv_event_t *e) {
    /* A swipe that starts on a row still releases over it; ignore gestures so
     * only genuine taps select (mirrors the menu tile fix). */
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE)
        return;
    row_ctx_t *ctx = lv_event_get_user_data(e);
    snprintf(ctx->st->selected, sizeof(ctx->st->selected), "%s", ctx->host);
    repaint_rows(ctx->st);
}

static void assign_side(dev_side_t *side, const char *host) {
    snprintf(side->host, DEV_HOST_MAX, "%s", host);
    dev_graph_set_host(side->cpu, host);
    dev_graph_set_host(side->gpu, host);
    /* New host: the existing trace is unrelated data — flag the discontinuity. */
    dev_graph_mark_gap(side->cpu);
    dev_graph_mark_gap(side->gpu);
}

static void assign_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE)
        return;
    dev_state_t *st = lv_event_get_user_data(e);
    if (st->selected[0] == '\0')
        return; /* nothing selected: no-op */
    bool is_left = (bool)(uintptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    assign_side(is_left ? &st->left : &st->right, st->selected);
    repaint_rows(st);
}

/* Rebuild the row widgets to match the current (sorted) host list. Selection
 * and markers are re-applied afterwards by repaint_rows. */
static void rebuild_rows(dev_state_t *st) {
    for (int i = 0; i < st->row_count; i++) {
        if (st->rows[i])
            lv_obj_delete(st->rows[i]);
        st->rows[i] = NULL;
        st->row_lbls[i] = NULL;
    }

    int n = st->hosts.count;
    if (n > DEV_HOSTLIST_MAX)
        n = DEV_HOSTLIST_MAX;
    st->row_count = n;

    for (int i = 0; i < n; i++) {
        snprintf(st->row_ctx[i].host, DEV_HOST_MAX, "%s",
                 st->hosts.entries[i].host);
        st->row_ctx[i].st = st;

        lv_obj_t *btn = lv_button_create(st->list);
        lv_obj_set_size(btn, LV_PCT(100), ROW_H);
        lv_obj_set_style_bg_color(btn, COLOR_ROW, 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_pad_left(btn, 8, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(btn, row_cb, LV_EVENT_CLICKED, &st->row_ctx[i]);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        st->rows[i] = btn;
        st->row_lbls[i] = lbl;
    }
}

static lv_obj_t *make_assign_btn(lv_obj_t *parent, const char *text,
                                 bool is_left, lv_event_cb_t cb,
                                 void *user_data) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 108, 48);
    lv_obj_set_style_bg_color(btn, COLOR_ASSIGN, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    /* Tag the button with its side so one callback serves both. */
    lv_obj_set_user_data(btn, (void *)(uintptr_t)is_left);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);
    return btn;
}

static void make_center(dev_state_t *st, lv_obj_t *parent) {
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_set_size(col, CENTER_W, LV_PCT(100));
    lv_obj_set_style_bg_color(col, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 6, 0);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(col);
    lv_label_set_text(title, "Dev");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, COLOR_HOST, 0);

    /* Assign controls: pick a row, then commit it left or right. */
    lv_obj_t *btn_row = lv_obj_create(col);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_row, LV_OBJ_FLAG_GESTURE_BUBBLE);
    make_assign_btn(btn_row, LV_SYMBOL_LEFT " L", true, assign_cb, st);
    make_assign_btn(btn_row, "R " LV_SYMBOL_RIGHT, false, assign_cb, st);

    /* Scrollable host list fills the remaining height. */
    lv_obj_t *list = lv_obj_create(col);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 2, 0);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_add_flag(list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    st->list = list;
}

static void build_screen(kd_mode_t *self) {
    dev_state_t *st = self->state;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 6, 0);
    lv_obj_set_style_pad_column(scr, 6, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* Mirrored order: CPU/RAM outer, GPU/VRAM inner on both sides. */
    st->left.cpu = dev_graph_create(scr, DEV_GRAPH_CPU_RAM);
    st->left.gpu = dev_graph_create(scr, DEV_GRAPH_GPU_VRAM);
    make_center(st, scr);
    st->right.gpu = dev_graph_create(scr, DEV_GRAPH_GPU_VRAM);
    st->right.cpu = dev_graph_create(scr, DEV_GRAPH_CPU_RAM);

    self->screen = scr;
}

/* Refresh the selector list from discovery: merge the fresh snapshot into the
 * debounced model (keeping the selected + assigned hosts), rebuild rows only
 * when membership changed, and always repaint markers/highlight. */
static void selector_refresh(dev_state_t *st) {
    char fresh[TELEMETRY_HOSTS_MAX][DEV_HOST_MAX];
    int n = telemetry_hosts(fresh, TELEMETRY_HOSTS_MAX);

    const char *keep[3] = {st->selected, st->left.host, st->right.host};
    bool changed = dev_hostlist_merge(&st->hosts, fresh, n, keep, 3);
    if (changed)
        rebuild_rows(st);
    repaint_rows(st);
}

static void poll_side(dev_side_t *side) {
    if (side->host[0] == '\0')
        return;
    dev_sample_t s;
    if (telemetry_get_sample(side->host, &s) == TELEMETRY_OK) {
        dev_graph_update(side->cpu, &s);
        dev_graph_update(side->gpu, &s);
    }
}

static void activate(kd_mode_t *self) {
    if (!self->screen)
        build_screen(self);
    dev_state_t *st = self->state;
    /* Force discovery + a poll on the next tick. */
    st->last_poll = 0;
    st->last_discover = 0;
    /* Returning to the mode after time away: the trace resumes with a time gap,
     * so drop a start line on each assigned side's charts. */
    if (st->left.host[0] != '\0') {
        dev_graph_mark_gap(st->left.cpu);
        dev_graph_mark_gap(st->left.gpu);
    }
    if (st->right.host[0] != '\0') {
        dev_graph_mark_gap(st->right.cpu);
        dev_graph_mark_gap(st->right.gpu);
    }
}

static void tick(kd_mode_t *self) {
    dev_state_t *st = self->state;
    if (!self->screen)
        return;

    if (st->last_discover == 0 ||
        lv_tick_elaps(st->last_discover) >= DEV_DISCOVER_MS) {
        telemetry_discover_step();
        selector_refresh(st);
        st->last_discover = lv_tick_get();
    }

    if (st->last_poll == 0 || lv_tick_elaps(st->last_poll) >= DEV_POLL_MS) {
        poll_side(&st->left);
        poll_side(&st->right);
        st->last_poll = lv_tick_get();
    }
}

kd_mode_t *dev_mode_create(const char *id, const char *title) {
    kd_mode_t *m = calloc(1, sizeof(*m));
    dev_state_t *st = calloc(1, sizeof(*st));
    dev_hostlist_init(&st->hosts);
    m->id = id;
    m->title = title;
    m->state = st;
    m->activate = activate;
    m->deactivate = NULL;
    m->tick = tick;
    return m;
}
