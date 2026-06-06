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

#include <stdlib.h>

#include "gol.h"
#include "lvgl.h"

typedef struct {
    lv_obj_t *canvas;
    uint32_t *cbuf;   /* full-screen XRGB8888 pixels, disp_w * disp_h */
    int       disp_w;
    int       disp_h;

    gol_t     gol;
    bool      has_grid;
    uint32_t  rng;       /* PRNG state for settings + seeding */
    uint32_t  last_step; /* lv_tick at last generation advance */
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
    return cfg;
}

static void render(gol_mode_state_t *st) {
    int stride = st->disp_w; /* pixels per row (buffer is tightly packed) */
    /* Background: opaque black. */
    for (int i = 0; i < st->disp_w * st->disp_h; i++)
        st->cbuf[i] = 0xFF000000u;

    const gol_t *g = &st->gol;
    int block = g->cfg.cell_size + g->cfg.padding;
    int trail_turns = g->cfg.trail_turns > 0 ? g->cfg.trail_turns : 1;

    for (int cy = 0; cy < g->rows; cy++) {
        for (int cx = 0; cx < g->cols; cx++) {
            uint32_t color;
            if (gol_get(g, cx, cy)) {
                color = 0xFF00FF00u; /* live: full green */
            } else {
                uint8_t t = gol_trail(g, cx, cy);
                if (t == 0)
                    continue; /* dark cell: leave background */
                uint32_t green = (uint32_t)(255 * t / trail_turns);
                color = 0xFF000000u | (green << 8);
            }

            int px0 = cx * block;
            int py0 = cy * block;
            for (int dy = 0; dy < g->cfg.cell_size; dy++) {
                int py = py0 + dy;
                if (py >= st->disp_h)
                    break;
                uint32_t *row = st->cbuf + (size_t)py * stride + px0;
                for (int dx = 0; dx < g->cfg.cell_size; dx++) {
                    if (px0 + dx >= st->disp_w)
                        break;
                    row[dx] = color;
                }
            }
        }
    }

    lv_obj_invalidate(st->canvas);
}

/* Re-seed the board with fresh random settings sized to the current display. */
static void reseed(gol_mode_state_t *st) {
    gol_settings_t cfg = random_settings(&st->rng);
    int block = cfg.cell_size + cfg.padding;
    int cols = st->disp_w / block;
    int rows = st->disp_h / block;
    if (cols < 1)
        cols = 1;
    if (rows < 1)
        rows = 1;

    if (st->has_grid)
        gol_free(&st->gol);
    gol_init(&st->gol, cols, rows, &cfg);
    st->has_grid = true;
    gol_seed(&st->gol, &st->rng);
    render(st);
    st->last_step = lv_tick_get();
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
    lv_obj_t *canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(canvas, st->cbuf, st->disp_w, st->disp_h,
                         LV_COLOR_FORMAT_XRGB8888);
    lv_obj_align(canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    /* Swipes that begin on the canvas must still drive shell navigation. */
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_GESTURE_BUBBLE);
    st->canvas = canvas;

    self->screen = scr;
}

static void activate(kd_mode_t *self) {
    gol_mode_state_t *st = self->state;
    if (!self->screen)
        build_screen(self);
    /* Fresh randomized run on every activation (R9). */
    st->rng ^= lv_tick_get() * 2654435761u;
    if (st->rng == 0)
        st->rng = 0x1234567u;
    reseed(st);
}

static void tick(kd_mode_t *self) {
    gol_mode_state_t *st = self->state;
    if (!st->has_grid)
        return;
    uint32_t speed = st->gol.cfg.speed_ms;
    if (lv_tick_elaps(st->last_step) < speed)
        return;
    gol_step(&st->gol);
    render(st);
    st->last_step = lv_tick_get();
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
