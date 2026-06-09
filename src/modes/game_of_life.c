/**
 * @file game_of_life.c
 * Full-screen Conway's Game of Life on an LVGL canvas. The simulation lives in
 * the pure core (gol.c); this file owns the canvas, the per-activation random
 * settings, the generation timer, and pixel rendering.
 *
 * Rendering writes the owned XRGB8888 buffer directly (one uint32_t per pixel)
 * and invalidates the canvas once per generation. Live cells are drawn green;
 * when trails are enabled, recently-dead cells fade by scaling the green
 * channel by their remaining trail intensity.
 */
#include "modes/game_of_life.h"

#include <stdio.h>
#include <stdlib.h>

#include "gol.h"
#include "lvgl.h"
#include "redis.h"

/* Grace period after a cycle is first detected before auto-Restart (R-C8). */
#define GOL_CYCLE_GRACE_MS 30000

typedef struct {
    lv_obj_t *canvas;
    uint32_t *cbuf;   /* full-screen XRGB8888 pixels, disp_w * disp_h */
    int       disp_w;
    int       disp_h;

    gol_t     boards[3];  /* board 0 (or R/G/B when rgb); see board_count */
    int       board_count; /* 1 (single green) or 3 (rgb tri-board) */
    uint32_t  rng;        /* PRNG state for settings + seeding */
    uint32_t  last_step;  /* lv_tick at last generation advance */
    uint32_t  generation; /* generations since the last (re)seed */

    bool        cycle_detect; /* spot still-lifes/oscillators (on when !rgb) */
    gol_cycle_t cycle;        /* recent-generation hash ring */
    bool        cycle_armed;  /* a cycle was seen; grace countdown running */
    uint32_t    cycle_since;  /* lv_tick the countdown (re)started at */

    lv_point_t press_pt;   /* canvas press point, captured on PRESSED */
    lv_obj_t  *menu;       /* control overlay root; NULL when closed */
    lv_obj_t  *info_label; /* settings + generation readout in the overlay */
    bool       menu_open;
} gol_mode_state_t;

/* Pick fresh randomized settings within sensible MVP ranges. The cell-size
 * floor keeps the worst-case grid (and per-frame work) bounded on the panel;
 * this is the knob to tune on device. */
static gol_settings_t random_settings(uint32_t *rng) {
    gol_settings_t cfg;
    cfg.cell_size = 2 + (int)(gol_rand_u32(rng) % 29);          /* 2..30 px */
    cfg.padding = (int)(gol_rand_u32(rng) % 3);               /* 0..2 px */
    if (cfg.padding >= cfg.cell_size)
        cfg.padding = cfg.cell_size - 1;
    /* density centered near a third: 0.15..0.50 */
    cfg.density = 0.15 + (gol_rand_u32(rng) / 4294967295.0) * 0.35;
    cfg.trail = (gol_rand_u32(rng) % 2) != 0;                 /* 50-50 trail on/off */
    cfg.trail_turns = 3 + (int)(gol_rand_u32(rng) % 8);       /* 3..10 gens */
    cfg.speed_ms = 80 + (int)(gol_rand_u32(rng) % 620);       /* 80..700 ms */
    /* Drawn last so adding rgb does not shift the existing settings' stream. */
    cfg.rgb = (gol_rand_u32(rng) % 5) == 0;                   /* ~20% chance */
    return cfg;
}

static void render(gol_mode_state_t *st) {
    if (!st->cbuf)
        return; /* canvas buffer alloc failed; degrade to a blank screen */
    int stride = st->disp_w; /* pixels per row (buffer is tightly packed) */
    /* Background: opaque black. */
    for (int i = 0; i < st->disp_w * st->disp_h; i++)
        st->cbuf[i] = 0xFF000000u;

    if (st->board_count >= 1) {
        const gol_t *g0 = &st->boards[0];
        int block = g0->cfg.cell_size + g0->cfg.padding;
        int trail_turns = g0->cfg.trail_turns > 0 ? g0->cfg.trail_turns : 1;
        int n = st->board_count;

        for (int cy = 0; cy < g0->rows; cy++) {
            for (int cx = 0; cx < g0->cols; cx++) {
                uint8_t c[3] = {0, 0, 0};
                for (int b = 0; b < n; b++) {
                    const gol_t *g = &st->boards[b];
                    c[b] = gol_channel_intensity(gol_get(g, cx, cy),
                                                 gol_trail(g, cx, cy),
                                                 trail_turns);
                }
                uint32_t color = gol_compose_pixel(c[0], c[1], c[2], n);
                if (color == 0xFF000000u)
                    continue; /* dark cell: leave background */

                int px0 = cx * block;
                int py0 = cy * block;
                for (int dy = 0; dy < g0->cfg.cell_size; dy++) {
                    int py = py0 + dy;
                    if (py >= st->disp_h)
                        break;
                    uint32_t *row = st->cbuf + (size_t)py * stride + px0;
                    for (int dx = 0; dx < g0->cfg.cell_size; dx++) {
                        if (px0 + dx >= st->disp_w)
                            break;
                        row[dx] = color;
                    }
                }
            }
        }
    }

    lv_obj_invalidate(st->canvas);
}

/* Re-seed the board(s) with the given settings, sized to the current display.
 * Allocates 1 board normally, or 3 independent boards when rgb is set. */
static void reseed(gol_mode_state_t *st, const gol_settings_t *cfg) {
    int block = cfg->cell_size + cfg->padding;
    int cols = st->disp_w / block;
    int rows = st->disp_h / block;
    if (cols < 1)
        cols = 1;
    if (rows < 1)
        rows = 1;

    /* Free every slot before re-allocating: the board count changes when rgb is
     * toggled, and gol_free is safe on a zeroed/already-freed board. */
    for (int b = 0; b < 3; b++)
        gol_free(&st->boards[b]);

    int n = cfg->rgb ? 3 : 1;
    bool ok = true;
    for (int b = 0; b < n; b++) {
        if (!gol_init(&st->boards[b], cols, rows, cfg)) {
            ok = false;
            break;
        }
        gol_seed(&st->boards[b], &st->rng); /* independent population per board */
    }
    if (ok) {
        st->board_count = n;
    } else {
        /* Degrade to a blank screen (matches the cbuf-failure path); never
         * silently fall back to a single board, which would drop rgb. */
        for (int b = 0; b < 3; b++)
            gol_free(&st->boards[b]);
        st->board_count = 0;
        fprintf(stderr, "game_of_life: board alloc failed (%dx%d x%d)\n", cols,
                rows, n);
    }
    /* Cycle detection runs only on the single (non-rgb) board; reset its ring
     * on every reseed so stale hashes can't trigger a false Restart (R-C6). */
    st->cycle_detect = !cfg->rgb;
    gol_cycle_reset(&st->cycle);
    st->cycle_armed = false;
    st->generation = 0;
    render(st);
    st->last_step = lv_tick_get();
}

/* Restart: fresh randomized settings + a new population. Also the activation
 * path (R-A9). */
static void roll_and_reseed(gol_mode_state_t *st) {
    /* Fresh randomized run on every (re)start. */
    st->rng ^= lv_tick_get() * 2654435761u;
    if (st->rng == 0)
        st->rng = 0x1234567u;
    /* Randomize, then overlay any one-shot settings injected via Redis; absent
     * fields keep their randomized values and the injection key is cleared. */
    gol_settings_t cfg = random_settings(&st->rng);
    redis_apply_gol_settings(&cfg);
    if (cfg.padding >= cfg.cell_size)
        cfg.padding = cfg.cell_size - 1;
    reseed(st, &cfg);
}

/* Reset: a new population with the current settings unchanged (R-A8). */
static void reseed_same(gol_mode_state_t *st) {
    if (st->board_count < 1) {
        roll_and_reseed(st);
        return;
    }
    /* Copy the settings before reseed() frees/re-inits the boards. */
    gol_settings_t cfg = st->boards[0].cfg;
    reseed(st, &cfg);
}

/* Refresh the overlay's settings + generation readout. */
static void update_info_label(gol_mode_state_t *st) {
    if (!st->info_label || st->board_count < 1)
        return;
    const gol_settings_t *c = &st->boards[0].cfg;
    char buf[256];
    snprintf(buf, sizeof(buf),
             "cell %d  pad %d\n"
             "density %.2f\n"
             "trail %s  turns %d\n"
             "speed %d ms\n"
             "rgb %s\n"
             "gen %u",
             c->cell_size, c->padding, c->density, c->trail ? "on" : "off",
             c->trail_turns, c->speed_ms, c->rgb ? "on" : "off", st->generation);
    lv_label_set_text(st->info_label, buf);
}

static void close_menu(gol_mode_state_t *st) {
    if (st->menu) {
        lv_obj_delete(st->menu);
        st->menu = NULL;
        st->info_label = NULL;
    }
    st->menu_open = false;
}

/* The simulation never stops; the buttons just re-seed and dismiss the menu. */
static void reset_cb(lv_event_t *e) {
    gol_mode_state_t *st = lv_event_get_user_data(e);
    if (lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_NONE)
        return;
    reseed_same(st);
    close_menu(st);
}

static void restart_cb(lv_event_t *e) {
    gol_mode_state_t *st = lv_event_get_user_data(e);
    if (lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_NONE)
        return;
    roll_and_reseed(st);
    close_menu(st);
}

static void cancel_cb(lv_event_t *e) {
    gol_mode_state_t *st = lv_event_get_user_data(e);
    if (lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_NONE)
        return;
    close_menu(st);
}

static void make_menu_button(lv_obj_t *parent, const char *text,
                             lv_event_cb_t cb, gol_mode_state_t *st) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2b3340), LV_PART_MAIN);
    /* A swipe that begins on a button must still bubble to shell nav (R-A13). */
    lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, st);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xeaf0fb), LV_PART_MAIN);
    lv_obj_center(lbl);
}

/* Build the opaque control overlay over the right quarter. The simulation keeps
 * running underneath; the panel simply covers those cells (R-A4/A5). */
static void open_menu(gol_mode_state_t *st) {
    if (st->menu_open || !st->canvas)
        return;
    lv_obj_t *scr = lv_obj_get_screen(st->canvas);
    if (!scr)
        return;

    int w = st->disp_w / 4;
    if (w < 1)
        w = 1;

    lv_obj_t *panel = lv_obj_create(scr);
    lv_obj_set_size(panel, w, st->disp_h);
    lv_obj_align(panel, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x12151c), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(panel, 8, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *info = lv_label_create(panel);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(info, lv_color_hex(0xc8d0e0), LV_PART_MAIN);
    lv_obj_add_flag(info, LV_OBJ_FLAG_GESTURE_BUBBLE);

    st->menu = panel;
    st->info_label = info;
    st->menu_open = true;
    update_info_label(st);

    make_menu_button(panel, "Reset", reset_cb, st);
    make_menu_button(panel, "Restart", restart_cb, st);
    make_menu_button(panel, "Cancel", cancel_cb, st);
}

/* Capture the press point; the open decision happens on CLICKED so the gesture
 * detector has had a chance to classify a swipe (R-A1/A3). */
static void canvas_pressed_cb(lv_event_t *e) {
    gol_mode_state_t *st = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev)
        lv_indev_get_point(indev, &st->press_pt);
}

static void canvas_clicked_cb(lv_event_t *e) {
    gol_mode_state_t *st = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    /* A swipe that starts on the canvas drives shell nav, not the menu. */
    if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE)
        return;
    if (st->menu_open)
        return;
    /* Only taps landing in the right quarter open the menu (R-A1/A2). */
    if (st->press_pt.x >= st->disp_w * 3 / 4)
        open_menu(st);
}

static void build_screen(kd_mode_t *self) {
    gol_mode_state_t *st = self->state;

    lv_display_t *disp = lv_display_get_default();
    st->disp_w = lv_display_get_horizontal_resolution(disp);
    st->disp_h = lv_display_get_vertical_resolution(disp);

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    st->cbuf = malloc((size_t)st->disp_w * st->disp_h * sizeof(uint32_t));
    if (!st->cbuf)
        fprintf(stderr, "game_of_life: canvas buffer alloc failed (%dx%d)\n",
                st->disp_w, st->disp_h);
    lv_obj_t *canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(canvas, st->cbuf, st->disp_w, st->disp_h,
                         LV_COLOR_FORMAT_XRGB8888);
    lv_obj_align(canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    /* Swipes that begin on the canvas must still drive shell navigation. */
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_GESTURE_BUBBLE);
    /* Taps open the control menu; CLICKABLE is needed for press/click events. */
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(canvas, canvas_pressed_cb, LV_EVENT_PRESSED, st);
    lv_obj_add_event_cb(canvas, canvas_clicked_cb, LV_EVENT_CLICKED, st);
    st->canvas = canvas;

    self->screen = scr;
}

static void activate(kd_mode_t *self) {
    gol_mode_state_t *st = self->state;
    if (!self->screen)
        build_screen(self);
    /* A re-activation always starts a fresh randomized run with no menu open. */
    close_menu(st);
    roll_and_reseed(st);
}

static void tick(kd_mode_t *self) {
    gol_mode_state_t *st = self->state;
    if (st->board_count < 1)
        return;
    uint32_t speed = st->boards[0].cfg.speed_ms;
    if (lv_tick_elaps(st->last_step) < speed)
        return;
    for (int b = 0; b < st->board_count; b++)
        gol_step(&st->boards[b]);
    st->generation++;
    render(st);
    st->last_step = lv_tick_get();
    /* The sim keeps running while the menu is open; keep the readout live. */
    if (st->menu_open)
        update_info_label(st);

    /* Cycle detection: only on the single-board (non-rgb) path (R-C1). */
    if (st->cycle_detect && st->board_count == 1) {
        if (gol_cycle_record(&st->cycle, &st->boards[0]) && !st->cycle_armed) {
            st->cycle_armed = true;
            st->cycle_since = lv_tick_get(); /* start the ~30s grace window */
        }
        if (st->cycle_armed) {
            if (st->menu_open)
                st->cycle_since = lv_tick_get(); /* pause+reset while menu open (R-C9) */
            else if (lv_tick_elaps(st->cycle_since) >= GOL_CYCLE_GRACE_MS)
                roll_and_reseed(st); /* auto-Restart with fresh settings (R-C8) */
        }
    }
}

kd_mode_t *game_of_life_mode_create(const char *id, const char *title) {
    kd_mode_t *m = calloc(1, sizeof(*m));
    gol_mode_state_t *st = calloc(1, sizeof(*st));
    st->rng = 0x2545F491u;
    m->id = id;
    m->title = title;
    m->state = st;
    m->activate = activate;
    m->deactivate = NULL;
    m->tick = tick;
    return m;
}
