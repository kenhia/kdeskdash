/**
 * @file golz.c
 * GoLZ content mode — full-screen two-faction Game of Life on an LVGL canvas.
 * The simulation lives in the pure core (golz.c/golz.h); this file owns the
 * canvas, the per-round randomized settings, the generation timer, pixel
 * rendering (living green, zombies red, own-color fading trails), the persistent
 * zombie-win counter readout, and terminal-state routing.
 *
 * Rendering writes the owned XRGB8888 buffer directly (one uint32_t per pixel,
 * via golz_compose_pixel) and invalidates the canvas once per generation.
 */
#include "modes/golz.h"

#include <stdio.h>
#include <stdlib.h>

#include "golz.h"
#include "lvgl.h"
#include "redis.h"

/* Grace period after a cycle/extinction is first detected before auto-restart,
 * mirroring the Game of Life mode. */
#define GOLZ_CYCLE_GRACE_MS 30000

typedef struct {
    lv_obj_t *canvas;
    uint32_t *cbuf; /* full-screen XRGB8888 pixels, disp_w * disp_h */
    int       disp_w;
    int       disp_h;

    golz_t   game;       /* pure two-faction simulation */
    bool     ready;      /* board allocated; false degrades to a blank screen */
    uint32_t rng;        /* PRNG state for settings + seeding */
    uint32_t last_step;  /* lv_tick at the last generation advance */

    long      wins;         /* in-memory mirror of the persistent win counter */
    bool      win_recorded; /* per-round latch: counter incremented once (R17) */
    lv_obj_t *wins_label;   /* corner "Zombie wins: N" readout + identity marker */

    bool     cycle_armed; /* a quiet-restart was seen; grace countdown running */
    uint32_t cycle_since; /* lv_tick the countdown (re)started at */
} golz_mode_state_t;

/* Pick fresh randomized settings for one round. The living-layer knobs mirror
 * the Game of Life roller; the GoLZ-specific fields are drawn LAST so their
 * addition never shifts the existing settings' PRNG stream. */
static void roll_settings(uint32_t *rng, gol_settings_t *living,
                          golz_settings_t *z) {
    living->cell_size = 2 + (int)(gol_rand_u32(rng) % 29); /* 2..30 px */
    living->padding = (int)(gol_rand_u32(rng) % 3);        /* 0..2 px */
    if (living->padding >= living->cell_size)
        living->padding = living->cell_size - 1;
    living->density = 0.15 + (gol_rand_u32(rng) / 4294967295.0) * 0.35;
    living->trail = (gol_rand_u32(rng) % 2) != 0;
    living->trail_turns = 3 + (int)(gol_rand_u32(rng) % 8); /* 3..10 gens */
    living->speed_ms = 80 + (int)(gol_rand_u32(rng) % 620); /* 80..700 ms */
    living->rgb = false; /* GoLZ is always two-color */
    /* GoLZ-specific, drawn last. */
    z->initial_count = 1 + (int)(gol_rand_u32(rng) % 5);          /* 1..5 */
    z->zombie_reinfect = (int)(gol_rand_u32(rng) % 101);         /* 0..100 % */
    z->zombie_spawn_chance = (int)(gol_rand_u32(rng) % 101);     /* 0..100 % */
    z->max_generations = 2000 + (int)(gol_rand_u32(rng) % 2000); /* 2000..3999 */
}

static void render(golz_mode_state_t *st) {
    if (!st->cbuf)
        return; /* canvas buffer alloc failed; degrade to a blank screen */
    int stride = st->disp_w;
    for (int i = 0; i < st->disp_w * st->disp_h; i++)
        st->cbuf[i] = 0xFF000000u; /* opaque black background */

    if (st->ready) {
        const golz_t *g = &st->game;
        int block = g->living.cfg.cell_size + g->living.cfg.padding;
        if (block < 1)
            block = 1;
        for (int cy = 0; cy < g->rows; cy++) {
            for (int cx = 0; cx < g->cols; cx++) {
                uint32_t color = golz_compose_pixel(g, cx, cy);
                if (color == 0xFF000000u)
                    continue; /* dark cell: leave background */

                int px0 = cx * block;
                int py0 = cy * block;
                for (int dy = 0; dy < g->living.cfg.cell_size; dy++) {
                    int py = py0 + dy;
                    if (py >= st->disp_h)
                        break;
                    uint32_t *row = st->cbuf + (size_t)py * stride + px0;
                    for (int dx = 0; dx < g->living.cfg.cell_size; dx++) {
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

/* Refresh the corner win-counter readout (also the GoLZ identity marker). */
static void update_wins_label(golz_mode_state_t *st) {
    if (!st->wins_label)
        return;
    char buf[48];
    snprintf(buf, sizeof(buf), "Zombie wins: %ld", st->wins);
    lv_label_set_text(st->wins_label, buf);
}

/* Roll fresh settings, overlay any one-shot Redis injection, and reseed. */
static void reseed(golz_mode_state_t *st) {
    st->rng ^= lv_tick_get() * 2654435761u;
    if (st->rng == 0)
        st->rng = 0x1234567u;

    gol_settings_t living;
    golz_settings_t z;
    roll_settings(&st->rng, &living, &z);
    /* Overlay one-shot injected settings; absent fields keep the randomized
     * values and the injection key is cleared. */
    redis_apply_gol_settings(&living);
    redis_apply_golz_settings(&z);
    living.rgb = false;
    if (living.padding >= living.cell_size)
        living.padding = living.cell_size - 1;

    int block = living.cell_size + living.padding;
    if (block < 1)
        block = 1;
    int cols = st->disp_w / block;
    int rows = st->disp_h / block;
    if (cols < 1)
        cols = 1;
    if (rows < 1)
        rows = 1;

    golz_free(&st->game);
    if (golz_init(&st->game, cols, rows, &living, &z, &st->rng)) {
        golz_seed(&st->game);
        st->ready = true;
    } else {
        st->ready = false;
        fprintf(stderr, "golz: board alloc failed (%dx%d)\n", cols, rows);
    }

    st->win_recorded = false;
    st->cycle_armed = false;
    st->last_step = lv_tick_get();
    update_wins_label(st);
    render(st);
}

/* Record one zombie win: increment the persistent counter exactly once per
 * round (latched), preferring the authoritative Redis count and falling back to
 * the in-memory mirror when Redis is unavailable (R17). */
static void record_win(golz_mode_state_t *st) {
    if (st->win_recorded)
        return;
    long n = redis_golz_incr_wins();
    if (n >= 0)
        st->wins = n;
    else
        st->wins++;
    st->win_recorded = true;
    update_wins_label(st);
}

/* Route the per-generation terminal decision. */
static void handle_terminal(golz_mode_state_t *st, golz_terminal_t t) {
    if (t == GOLZ_ZOMBIE_WIN) {
        record_win(st);
        reseed(st); /* Unit 6 replaces this with the victory lap + banner */
        return;
    }
    if (t == GOLZ_QUIET_RESTART) {
        /* Arm a grace window on first detection, then auto-restart, mirroring
         * the Game of Life cycle behavior. */
        if (!st->cycle_armed) {
            st->cycle_armed = true;
            st->cycle_since = lv_tick_get();
        } else if (lv_tick_elaps(st->cycle_since) >= GOLZ_CYCLE_GRACE_MS) {
            reseed(st);
        }
    }
}

static void build_screen(kd_mode_t *self) {
    golz_mode_state_t *st = self->state;

    lv_display_t *disp = lv_display_get_default();
    st->disp_w = lv_display_get_horizontal_resolution(disp);
    st->disp_h = lv_display_get_vertical_resolution(disp);

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    st->cbuf = malloc((size_t)st->disp_w * st->disp_h * sizeof(uint32_t));
    if (!st->cbuf)
        fprintf(stderr, "golz: canvas buffer alloc failed (%dx%d)\n", st->disp_w,
                st->disp_h);
    lv_obj_t *canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(canvas, st->cbuf, st->disp_w, st->disp_h,
                         LV_COLOR_FORMAT_XRGB8888);
    lv_obj_align(canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    /* Swipes that begin on the canvas must still drive shell navigation. */
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_GESTURE_BUBBLE);
    st->canvas = canvas;

    /* Corner win-counter readout (GoLZ identity marker; visible every round). */
    lv_obj_t *wins = lv_label_create(scr);
    lv_obj_set_style_text_font(wins, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(wins, lv_color_hex(0xff4d4d), LV_PART_MAIN);
    lv_obj_set_style_bg_color(wins, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wins, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_all(wins, 6, LV_PART_MAIN);
    lv_obj_align(wins, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_add_flag(wins, LV_OBJ_FLAG_GESTURE_BUBBLE);
    st->wins_label = wins;

    self->screen = scr;
}

static void activate(kd_mode_t *self) {
    golz_mode_state_t *st = self->state;
    if (!self->screen)
        build_screen(self);
    /* Restore the persisted win count into the in-memory mirror (0 when Redis
     * is absent), then start a fresh randomized round. */
    st->wins = redis_golz_get_wins(0);
    reseed(st);
}

static void tick(kd_mode_t *self) {
    golz_mode_state_t *st = self->state;
    if (!st->ready)
        return;
    uint32_t speed = st->game.living.cfg.speed_ms;
    if (lv_tick_elaps(st->last_step) < speed)
        return;

    golz_step(&st->game);
    golz_terminal_t t = golz_terminal(&st->game);
    render(st);
    st->last_step = lv_tick_get();
    handle_terminal(st, t);
}

kd_mode_t *golz_mode_create(const char *id, const char *title) {
    kd_mode_t *m = calloc(1, sizeof(*m));
    golz_mode_state_t *st = calloc(1, sizeof(*st));
    st->rng = 0x9E3779B9u;
    m->id = id;
    m->title = title;
    m->state = st;
    m->activate = activate;
    m->deactivate = NULL;
    m->tick = tick;
    return m;
}
