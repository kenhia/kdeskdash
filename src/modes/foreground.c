/**
 * @file foreground.c
 * Remote-foreground ("R4gnd") mode — see foreground.h.
 *
 * Layout (1920×440), claude-mode design language:
 *   [ app rail ][ 4×7 window grid, alphabetical by label, column-major ]
 *
 * The rail shows the VS Code glyph (TinyTTF) and, on overflow, page up/down
 * arrows. Each grid cell is a window: label (coloured by app — stable blue,
 * insiders green) over host (grey). Tapping a cell publishes a focus command to
 * kvscf on that window's host (kvscf_redis_focus), which foregrounds it.
 *
 * All model logic (JSON parse, sort, paging, colours, focus payload) lives in
 * the pure kvscf_feed core; Redis I/O in kvscf_redis; this file is LVGL glue.
 */
#include "modes/foreground.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvscf_feed.h"
#include "kvscf_redis.h"
#include "lvgl.h"
#include "src/libs/tiny_ttf/lv_tiny_ttf.h"

#define POLL_MS     1500
#define RAIL_W      180
#define RAIL_GLYPH  76
#define CELL_H      54
#define CELL_GAP    6
#define HOST_STRIP  22 /* width of the rotated-host tab on a cell's right edge */

/* VS Code, U+F0A1E (nf-md-microsoft_visual_studio_code), pre-encoded UTF-8. */
#define GLYPH_VSCODE "\xF3\xB0\xA8\x9E"

#define COLOR_BG        lv_color_hex(0x05070d)
#define COLOR_PANEL     lv_color_hex(0x0a0f1a)
#define COLOR_PANEL_HI  lv_color_hex(0x101726)
#define COLOR_HAIRLINE  lv_color_hex(0x1b2334)
#define COLOR_INK       lv_color_hex(0xe9edf6)
#define COLOR_SECONDARY lv_color_hex(0x8b95ab)
#define COLOR_MUTED     lv_color_hex(0x525d73)
#define COLOR_ACCENT    lv_color_hex(0xcf6b4a) /* claude coral */
#define COLOR_HOST      lv_color_hex(0x969696) /* cleo-side host grey */
#define COLOR_VSCODE    lv_color_hex(0x60A5EB) /* rail glyph */

typedef struct fg_state fg_state_t;

typedef struct {
    fg_state_t *st;
    int         slot; /* 0..KV_PER_PAGE-1 */
} cell_ctx_t;

struct fg_state {
    const char *ttf_path;

    void      *ttf_data;
    size_t     ttf_size;
    lv_font_t *rail_font;

    kvscf_instance_t items[KV_INSTANCES_MAX];
    int              count;
    int              page;
    uint32_t         last_poll;

    /* widgets */
    lv_obj_t  *count_lbl;
    lv_obj_t  *page_nav;  /* container for up/down (hidden unless overflow) */
    lv_obj_t  *grid;
    lv_obj_t  *cells[KV_PER_PAGE];
    lv_obj_t  *cell_label[KV_PER_PAGE];
    lv_obj_t  *cell_host[KV_PER_PAGE];
    cell_ctx_t cell_ctx[KV_PER_PAGE];
    lv_obj_t  *banner; /* centered overlay for empty/unavailable */
    lv_obj_t  *toast;  /* last focus action / no-token note */
    bool       token_checked; /* the no-token note is set once, post-init */
};

/* Cells are stored column-major (slot = col*KV_ROWS + row) and each column is
 * its own flex container, so slot order *is* the alphabetical reading order
 * (down each column) and hidden empties always trail at a column's bottom. */
static int slot_to_item(const fg_state_t *st, int slot) {
    return st->page * KV_PER_PAGE + slot;
}

static int page_count(const fg_state_t *st) {
    return kvscf_page_count(st->count, KV_PER_PAGE);
}

static bool is_gesture(void) {
    lv_indev_t *indev = lv_indev_active();
    return indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE;
}

/* ---- repaint ---------------------------------------------------------- */

static void set_toast(fg_state_t *st, const char *msg, lv_color_t color) {
    lv_label_set_text(st->toast, msg);
    lv_obj_set_style_text_color(st->toast, color, 0);
}

static void repaint_grid(fg_state_t *st) {
    for (int s = 0; s < KV_PER_PAGE; s++) {
        int idx = slot_to_item(st, s);
        if (idx >= st->count) {
            lv_obj_add_flag(st->cells[s], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const kvscf_instance_t *in = &st->items[idx];
        lv_obj_clear_flag(st->cells[s], LV_OBJ_FLAG_HIDDEN);
        char label[KV_LABEL_MAX];
        kvscf_display_label(in, label, sizeof(label));
        lv_label_set_text(st->cell_label[s], label);
        lv_obj_set_style_text_color(st->cell_label[s],
                                    lv_color_hex(kvscf_app_color(in->app)), 0);
        lv_label_set_text(st->cell_host[s], kvscf_display_host(in));
    }

    /* Page nav + counter. */
    int pages = page_count(st);
    if (pages > 1)
        lv_obj_clear_flag(st->page_nav, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(st->page_nav, LV_OBJ_FLAG_HIDDEN);

    char c[32];
    if (pages > 1)
        snprintf(c, sizeof(c), "%d  ·  %d/%d", st->count, st->page + 1, pages);
    else
        snprintf(c, sizeof(c), "%d", st->count);
    lv_label_set_text(st->count_lbl, c);
}

/* Show a centered banner instead of the grid (empty / unavailable). */
static void show_banner(fg_state_t *st, const char *msg) {
    lv_label_set_text(st->banner, msg);
    lv_obj_clear_flag(st->banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(st->grid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(st->page_nav, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(st->count_lbl, "");
}

static void hide_banner(fg_state_t *st) {
    lv_obj_add_flag(st->banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(st->grid, LV_OBJ_FLAG_HIDDEN);
}

static void refresh(fg_state_t *st) {
    /* Evaluate the (static) token posture once, here rather than in
     * build_screen — the feed handle is initialised after shell_start, so
     * build can run before kvscf_redis_init and would read "no token". */
    if (!st->token_checked) {
        st->token_checked = true;
        if (!kvscf_redis_have_token())
            set_toast(st, "no token " LV_SYMBOL_WARNING " view only", COLOR_MUTED);
    }

    st->count = kvscf_redis_refresh(st->items, KV_INSTANCES_MAX);
    st->page = kvscf_clamp_page(st->page, st->count, KV_PER_PAGE);

    if (!kvscf_redis_reachable()) {
        show_banner(st, "kvscf feed unavailable");
        return;
    }
    if (st->count == 0) {
        show_banner(st, "no active editors");
        return;
    }
    hide_banner(st);
    repaint_grid(st);
}

/* ---- events ----------------------------------------------------------- */

static void cell_cb(lv_event_t *e) {
    if (is_gesture())
        return;
    cell_ctx_t *ctx = lv_event_get_user_data(e);
    fg_state_t *st = ctx->st;
    int idx = slot_to_item(st, ctx->slot);
    if (idx >= st->count)
        return;
    const kvscf_instance_t *in = &st->items[idx];

    if (!kvscf_redis_have_token()) {
        set_toast(st, "focus disabled — no token", COLOR_ACCENT);
        return;
    }
    char msg[96];
    if (kvscf_redis_focus(in->host, in->id, false)) {
        snprintf(msg, sizeof(msg), LV_SYMBOL_UP " %s", in->label);
        set_toast(st, msg, COLOR_ACCENT);
    } else {
        set_toast(st, "focus failed", COLOR_ACCENT);
    }
}

static void page_step_cb(lv_event_t *e) {
    if (is_gesture())
        return;
    fg_state_t *st = lv_event_get_user_data(e);
    int delta = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    st->page = kvscf_clamp_page(st->page + delta, st->count, KV_PER_PAGE);
    repaint_grid(st);
}

/* ---- construction ----------------------------------------------------- */

static bool load_ttf(fg_state_t *st) {
    FILE *f = fopen(st->ttf_path, "rb");
    if (!f)
        return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    if (sz <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    void *buf = malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return false;
    }
    st->ttf_data = buf;
    st->ttf_size = (size_t)sz;
    st->rail_font = lv_tiny_ttf_create_data(buf, (size_t)sz, RAIL_GLYPH);
    return st->rail_font != NULL;
}

static lv_obj_t *make_nav_btn(lv_obj_t *parent, const char *sym, int delta,
                              fg_state_t *st) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 64, 44);
    lv_obj_set_style_bg_color(btn, COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, COLOR_PANEL_HI, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, COLOR_HAIRLINE, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_user_data(btn, (void *)(intptr_t)delta);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(btn, page_step_cb, LV_EVENT_CLICKED, st);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_color(l, COLOR_INK, 0);
    lv_obj_center(l);
    return btn;
}

static void build_rail(fg_state_t *st, lv_obj_t *parent) {
    lv_obj_t *rail = lv_obj_create(parent);
    lv_obj_set_size(rail, RAIL_W, LV_PCT(100));
    lv_obj_set_style_bg_color(rail, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(rail, COLOR_HAIRLINE, 0);
    lv_obj_set_style_border_width(rail, 1, 0);
    lv_obj_set_style_border_side(rail, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_pad_all(rail, 10, 0);
    lv_obj_set_style_pad_row(rail, 8, 0);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(rail, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* App glyph (VS Code). Falls back to text if the font failed to load. */
    lv_obj_t *icon = lv_label_create(rail);
    if (st->rail_font) {
        lv_obj_set_style_text_font(icon, st->rail_font, 0);
        lv_label_set_text(icon, GLYPH_VSCODE);
    } else {
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
        lv_label_set_text(icon, "VS Code");
    }
    lv_obj_set_style_text_color(icon, COLOR_VSCODE, 0);

    st->count_lbl = lv_label_create(rail);
    lv_obj_set_style_text_font(st->count_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(st->count_lbl, COLOR_SECONDARY, 0);
    lv_label_set_text(st->count_lbl, "");

    /* Page up/down — hidden unless the list overflows one page. */
    st->page_nav = lv_obj_create(rail);
    lv_obj_remove_style_all(st->page_nav);
    lv_obj_set_size(st->page_nav, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(st->page_nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(st->page_nav, 8, 0);
    lv_obj_clear_flag(st->page_nav, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(st->page_nav, LV_OBJ_FLAG_GESTURE_BUBBLE);
    make_nav_btn(st->page_nav, LV_SYMBOL_UP, -1, st);
    make_nav_btn(st->page_nav, LV_SYMBOL_DOWN, +1, st);
    lv_obj_add_flag(st->page_nav, LV_OBJ_FLAG_HIDDEN);

    st->toast = lv_label_create(rail);
    lv_obj_set_style_text_font(st->toast, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->toast, COLOR_MUTED, 0);
    lv_label_set_long_mode(st->toast, LV_LABEL_LONG_DOT);
    lv_obj_set_width(st->toast, RAIL_W - 20);
    lv_label_set_text(st->toast, "");
}

static void build_cell(fg_state_t *st, lv_obj_t *colbox, int s) {
    lv_obj_t *cell = lv_button_create(colbox);
    lv_obj_set_size(cell, LV_PCT(100), CELL_H);
    lv_obj_set_style_bg_color(cell, COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cell, COLOR_PANEL_HI, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(cell, COLOR_HAIRLINE, LV_PART_MAIN);
    lv_obj_set_style_border_color(cell, COLOR_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(cell, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(cell, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(cell, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_right(cell, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(cell, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_GESTURE_BUBBLE);
    st->cell_ctx[s].st = st;
    st->cell_ctx[s].slot = s;
    lv_obj_add_event_cb(cell, cell_cb, LV_EVENT_CLICKED, &st->cell_ctx[s]);

    /* Big label fills the row height (host moved to the side); grows to take the
     * width left of the host strip. */
    lv_obj_t *label = lv_label_create(cell);
    lv_obj_set_flex_grow(label, 1);
    /* Pin to one line so an over-long title ellipsizes instead of wrapping to a
     * second line that overflows the cell (LONG_DOT wraps before dotting). */
    lv_obj_set_height(label, lv_font_montserrat_28.line_height);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(label, COLOR_INK, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, "");

    /* Host as a 90°-clockwise tab on the right edge: rotated about its own
     * centre inside a fixed-width strip so the vertical text stays centred. */
    lv_obj_t *strip = lv_obj_create(cell);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, HOST_STRIP, LV_PCT(100));
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *host = lv_label_create(strip);
    lv_obj_set_style_text_font(host, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(host, COLOR_HOST, 0);
    lv_obj_center(host);
    lv_obj_set_style_transform_pivot_x(host, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(host, lv_pct(50), 0);
    lv_obj_set_style_transform_rotation(host, 900, 0); /* 90° clockwise */
    lv_label_set_text(host, "");

    st->cells[s] = cell;
    st->cell_label[s] = label;
    st->cell_host[s] = host;
}

static void build_grid(fg_state_t *st, lv_obj_t *parent) {
    /* Outer row of KV_COLS column containers; each column is a top-aligned flex
     * column of KV_ROWS cells. Column-major fill (slot = col*KV_ROWS + row)
     * means the alphabetical order reads down each column and hidden empties
     * only ever trail at a column's bottom (no reflow scramble). */
    lv_obj_t *grid = lv_obj_create(parent);
    lv_obj_remove_style_all(grid);
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_height(grid, LV_PCT(100));
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(grid, CELL_GAP, 0);
    lv_obj_set_style_pad_all(grid, 8, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_GESTURE_BUBBLE);
    st->grid = grid;

    for (int col = 0; col < KV_COLS; col++) {
        lv_obj_t *colbox = lv_obj_create(grid);
        lv_obj_remove_style_all(colbox);
        lv_obj_set_flex_grow(colbox, 1);
        lv_obj_set_height(colbox, LV_PCT(100));
        lv_obj_set_flex_flow(colbox, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(colbox, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(colbox, CELL_GAP, 0);
        lv_obj_clear_flag(colbox, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(colbox, LV_OBJ_FLAG_GESTURE_BUBBLE);
        for (int row = 0; row < KV_ROWS; row++)
            build_cell(st, colbox, col * KV_ROWS + row);
    }

    /* Centered banner overlay (empty / unavailable), hidden by default. */
    st->banner = lv_label_create(parent);
    lv_obj_set_style_text_font(st->banner, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(st->banner, COLOR_MUTED, 0);
    lv_obj_align(st->banner, LV_ALIGN_CENTER, RAIL_W / 2, 0);
    lv_obj_add_flag(st->banner, LV_OBJ_FLAG_HIDDEN);
}

static void build_screen(kd_mode_t *self) {
    fg_state_t *st = self->state;
    load_ttf(st); /* non-fatal: rail falls back to text */

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    build_rail(st, scr);
    build_grid(st, scr);

    self->screen = scr;
}

static void activate(kd_mode_t *self) {
    fg_state_t *st = self->state;
    if (!self->screen)
        build_screen(self);
    /* Refresh on the next tick (which runs after kvscf_redis_init), not here —
     * activate can run during shell_start before the feed handle exists. */
    st->last_poll = 0;
}

static void tick(kd_mode_t *self) {
    fg_state_t *st = self->state;
    if (!self->screen)
        return;
    if (st->last_poll == 0 || lv_tick_elaps(st->last_poll) >= POLL_MS) {
        refresh(st);
        st->last_poll = lv_tick_get();
    }
}

kd_mode_t *foreground_mode_create(const char *id, const char *title,
                                  const char *ttf_path) {
    kd_mode_t *m = calloc(1, sizeof(*m));
    fg_state_t *st = calloc(1, sizeof(*st));
    st->ttf_path = ttf_path;
    m->id = id;
    m->title = title;
    m->state = st;
    m->activate = activate;
    m->deactivate = NULL;
    m->tick = tick;
    return m;
}
