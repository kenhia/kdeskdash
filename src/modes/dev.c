/**
 * @file dev.c
 * Dev mode: mirrored four-chart host-telemetry view. Two hosts (left/right)
 * each drive a CPU/RAM and a GPU/VRAM chart, laid out
 * `[CPU/RAM][GPU/VRAM]  selector  [GPU/VRAM][CPU/RAM]` so GPU/VRAM is inner on
 * both sides. Telemetry is polled (~1 s) only while the mode is active.
 *
 * Host assignment here is a stub: the first two discovered hosts are pinned to
 * left/right. Unit 5 adds the interactive selector and Unit 6 persists the
 * assignments; Unit 7 adds liveness/offline/CPU-only handling.
 */
#include "modes/dev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "modes/dev_graph.h"
#include "telemetry.h"

#define DEV_POLL_MS     1000 /* per-host GET cadence */
#define DEV_DISCOVER_MS 5000 /* SCAN discovery cadence (slower than poll) */
#define CENTER_W        260  /* center selector column width (px) */

#define COLOR_BG   lv_color_hex(0x05070d)
#define COLOR_HOST lv_color_hex(0xcfe0f5)
#define COLOR_CAP  lv_color_hex(0x7c93b3)

typedef struct {
    lv_obj_t *cpu;
    lv_obj_t *gpu;
    char host[DEV_HOST_MAX];
} dev_side_t;

typedef struct {
    dev_side_t left;
    dev_side_t right;
    lv_obj_t  *left_host_lbl;
    lv_obj_t  *right_host_lbl;
    uint32_t   last_poll;
    uint32_t   last_discover;
} dev_state_t;

static lv_obj_t *make_center(dev_state_t *st, lv_obj_t *parent) {
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_set_size(col, CENTER_W, LV_PCT(100));
    lv_obj_set_style_bg_color(col, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 6, 0);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(col);
    lv_label_set_text(title, "Dev");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, COLOR_HOST, 0);

    lv_obj_t *l_cap = lv_label_create(col);
    lv_label_set_text(l_cap, "Left");
    lv_obj_set_style_text_font(l_cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l_cap, COLOR_CAP, 0);

    st->left_host_lbl = lv_label_create(col);
    lv_label_set_text(st->left_host_lbl, "(none)");
    lv_obj_set_style_text_font(st->left_host_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(st->left_host_lbl, COLOR_HOST, 0);

    lv_obj_t *r_cap = lv_label_create(col);
    lv_label_set_text(r_cap, "Right");
    lv_obj_set_style_text_font(r_cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(r_cap, COLOR_CAP, 0);

    st->right_host_lbl = lv_label_create(col);
    lv_label_set_text(st->right_host_lbl, "(none)");
    lv_obj_set_style_text_font(st->right_host_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(st->right_host_lbl, COLOR_HOST, 0);

    return col;
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

/* Pin the first two discovered hosts to the unassigned sides (Unit 4 stub). */
static void assign_hosts(dev_state_t *st) {
    char hosts[TELEMETRY_HOSTS_MAX][DEV_HOST_MAX];
    int n = telemetry_hosts(hosts, TELEMETRY_HOSTS_MAX);

    for (int i = 0; i < n; i++) {
        const char *h = hosts[i];
        if (st->left.host[0] != '\0' && strcmp(st->left.host, h) == 0)
            continue; /* already left */
        if (st->right.host[0] != '\0' && strcmp(st->right.host, h) == 0)
            continue; /* already right */
        if (st->left.host[0] == '\0') {
            snprintf(st->left.host, sizeof(st->left.host), "%s", h);
            dev_graph_set_host(st->left.cpu, h);
            dev_graph_set_host(st->left.gpu, h);
            lv_label_set_text(st->left_host_lbl, h);
        } else if (st->right.host[0] == '\0') {
            snprintf(st->right.host, sizeof(st->right.host), "%s", h);
            dev_graph_set_host(st->right.cpu, h);
            dev_graph_set_host(st->right.gpu, h);
            lv_label_set_text(st->right_host_lbl, h);
        } else {
            break; /* both sides assigned */
        }
    }
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
}

static void tick(kd_mode_t *self) {
    dev_state_t *st = self->state;
    if (!self->screen)
        return;

    if (st->last_discover == 0 || lv_tick_elaps(st->last_discover) >= DEV_DISCOVER_MS) {
        telemetry_discover_step();
        assign_hosts(st);
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
    m->id = id;
    m->title = title;
    m->state = st;
    m->activate = activate;
    m->deactivate = NULL;
    m->tick = tick;
    return m;
}
