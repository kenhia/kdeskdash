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

/* How long the "The Zombies Won" banner holds before auto re-roll (a tap
 * short-circuits it). */
#define GOLZ_BANNER_HOLD_MS 30000

/* Mode phases: the normal simulation, the zombie-win victory lap (zombies march
 * off the bottom and trails drain), and the win banner hold. */
typedef enum {
    GOLZ_PHASE_RUN = 0,
    GOLZ_PHASE_LAP,
    GOLZ_PHASE_BANNER,
} golz_phase_t;

typedef struct {
    lv_obj_t *canvas;
    uint32_t *cbuf; /* full-screen XRGB8888 pixels, disp_w * disp_h */
    int       disp_w;
    int       disp_h;

    golz_t   game;       /* pure two-faction simulation */
    bool     ready;      /* board allocated; false degrades to a blank screen */
    uint32_t rng;        /* PRNG state for settings + seeding */
    uint32_t last_step;  /* lv_tick at the last generation/lap advance */
    golz_phase_t phase;  /* run / victory lap / banner */

    long      wins;         /* in-memory mirror of the persistent win counter */
    bool      win_recorded; /* per-round latch: counter incremented once (R17) */
    lv_obj_t *wins_label;   /* corner "Zombie wins: N" readout + identity marker */

    bool     cycle_armed; /* a quiet-restart was seen; grace countdown running */
    uint32_t cycle_since; /* lv_tick the countdown (re)started at */

    lv_point_t press_pt;   /* canvas press point, captured on PRESSED */
    lv_obj_t  *menu;       /* control overlay root; NULL when closed */
    bool       menu_open;
    lv_obj_t  *banner;     /* win banner overlay root; NULL when hidden */
    uint32_t   banner_since; /* lv_tick the banner hold started */
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

static void close_menu(golz_mode_state_t *st) {
    if (st->menu) {
        lv_obj_delete(st->menu);
        st->menu = NULL;
    }
    st->menu_open = false;
}

static void close_banner(golz_mode_state_t *st) {
    if (st->banner) {
        lv_obj_delete(st->banner);
        st->banner = NULL;
    }
}

/* Tear down any open overlay; called before a fresh round so nothing leaks. */
static void close_overlays(golz_mode_state_t *st) {
    close_menu(st);
    close_banner(st);
}

/* (Re)initialise the board with the given settings at the current dimensions,
 * seed it, and reset the per-round run state. */
static void start_round(golz_mode_state_t *st, const gol_settings_t *living,
                        const golz_settings_t *z, int cols, int rows) {
    close_overlays(st);
    golz_free(&st->game);
    if (golz_init(&st->game, cols, rows, living, z, &st->rng)) {
        golz_seed(&st->game);
        st->ready = true;
    } else {
        st->ready = false;
        fprintf(stderr, "golz: board alloc failed (%dx%d)\n", cols, rows);
    }
    st->phase = GOLZ_PHASE_RUN;
    st->win_recorded = false;
    st->cycle_armed = false;
    st->last_step = lv_tick_get();
    update_wins_label(st);
    render(st);
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

    start_round(st, &living, &z, cols, rows);
}

/* Reset: a new population with the current settings and dimensions unchanged. */
static void reseed_same(golz_mode_state_t *st) {
    if (!st->ready) {
        reseed(st);
        return;
    }
    /* Copy settings/dimensions before start_round frees/re-inits the board. */
    gol_settings_t living = st->game.living.cfg;
    golz_settings_t z = st->game.cfg;
    start_round(st, &living, &z, st->game.cols, st->game.rows);
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

/* Route the per-generation terminal decision while running. */
static void handle_terminal(golz_mode_state_t *st, golz_terminal_t t) {
    if (t == GOLZ_ZOMBIE_WIN) {
        record_win(st);              /* R17: increment exactly once (latched) */
        st->phase = GOLZ_PHASE_LAP;  /* enter the victory lap (R20) */
        st->last_step = lv_tick_get();
        return;
    }
    if (t == GOLZ_QUIET_RESTART) {
        /* Arm a grace window on first detection, then auto-restart, mirroring
         * the Game of Life cycle behavior. The menu pauses the countdown. */
        if (!st->cycle_armed) {
            st->cycle_armed = true;
            st->cycle_since = lv_tick_get();
        } else if (st->menu_open) {
            st->cycle_since = lv_tick_get();
        } else if (lv_tick_elaps(st->cycle_since) >= GOLZ_CYCLE_GRACE_MS) {
            reseed(st);
        }
    }
}

/* One victory-lap step: every zombie marches strictly one row downward (no
 * vertical wrap); zombies that reach past the bottom row are removed. Trails
 * (red and any residual green) fade one turn. Bottom-up iteration moves each
 * zombie exactly once. Returns true once the board is empty of zombies and all
 * trails have drained to black (the lap always terminates within rows +
 * trail_turns steps). */
static bool lap_step(golz_mode_state_t *st) {
    golz_t *g = &st->game;
    int cols = g->cols, rows = g->rows;
    for (int y = rows - 1; y >= 0; y--) {
        for (int x = 0; x < cols; x++) {
            int i = y * cols + x;
            if (!g->zombies[i])
                continue;
            g->zombies[i] = 0;
            int ny = y + 1;
            if (ny < rows)
                g->zombies[ny * cols + x] = 1; /* march down */
            /* else: marched off the bottom -> removed */
        }
    }

    int tt = g->living.cfg.trail_turns;
    if (tt < 1)
        tt = 1;
    int n = cols * rows;
    bool any_zombie = false, any_trail = false;
    for (int i = 0; i < n; i++) {
        if (g->zombies[i]) {
            g->z_trail[i] = (uint8_t)tt;
            any_zombie = true;
        } else if (g->z_trail[i] > 0) {
            g->z_trail[i]--;
        }
        if (g->living.trail[i] > 0)
            g->living.trail[i]--;
        if (g->z_trail[i] > 0 || g->living.trail[i] > 0)
            any_trail = true;
    }
    return !any_zombie && !any_trail;
}

/* Short-circuit the banner hold back to a fresh round (tap or timeout). */
static void banner_clicked_cb(lv_event_t *e) {
    golz_mode_state_t *st = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE)
        return; /* a swipe over the banner still drives shell nav */
    reseed(st);
}

/* Show the full-screen "The Zombies Won" banner over the frozen board. */
static void show_banner(golz_mode_state_t *st) {
    st->phase = GOLZ_PHASE_BANNER;
    st->banner_since = lv_tick_get();
    if (!st->canvas)
        return;
    lv_obj_t *scr = lv_obj_get_screen(st->canvas);
    if (!scr)
        return;

    lv_obj_t *scrim = lv_obj_create(scr);
    lv_obj_set_size(scrim, st->disp_w, st->disp_h);
    lv_obj_align(scrim, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(scrim, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(scrim, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(scrim, 0, LV_PART_MAIN);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    /* A swipe over the banner must still reach the shell; a tap restarts. */
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scrim, banner_clicked_cb, LV_EVENT_CLICKED, st);

    char buf[64];
    snprintf(buf, sizeof(buf), "The Zombies Won\n%ld total", st->wins);
    lv_obj_t *lbl = lv_label_create(scrim);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xff4d4d), LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(lbl);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_GESTURE_BUBBLE);

    st->banner = scrim;
}

/* The control menu just reseeds and dismisses; the simulation never stops. */
static void reset_cb(lv_event_t *e) {
    golz_mode_state_t *st = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE)
        return;
    reseed_same(st);
}

static void restart_cb(lv_event_t *e) {
    golz_mode_state_t *st = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE)
        return;
    reseed(st);
}

static void cancel_cb(lv_event_t *e) {
    golz_mode_state_t *st = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE)
        return;
    close_menu(st);
}

static void make_menu_button(lv_obj_t *parent, const char *text,
                             lv_event_cb_t cb, golz_mode_state_t *st) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2b3340), LV_PART_MAIN);
    /* A swipe that begins on a button must still bubble to shell nav. */
    lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, st);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xeaf0fb), LV_PART_MAIN);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_center(lbl);
}

/* Build the opaque control overlay over the right quarter. The simulation keeps
 * running underneath. */
static void open_menu(golz_mode_state_t *st) {
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
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    st->menu = panel;
    st->menu_open = true;

    make_menu_button(panel, "Reset", reset_cb, st);
    make_menu_button(panel, "Restart", restart_cb, st);
    make_menu_button(panel, "Cancel", cancel_cb, st);
}

/* Capture the press point; the open decision happens on CLICKED so the gesture
 * detector has had a chance to classify a swipe. */
static void canvas_pressed_cb(lv_event_t *e) {
    golz_mode_state_t *st = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev)
        lv_indev_get_point(indev, &st->press_pt);
}

static void canvas_clicked_cb(lv_event_t *e) {
    golz_mode_state_t *st = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    /* A swipe that starts on the canvas drives shell nav, not the menu. */
    if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE)
        return;
    if (st->menu_open || st->phase != GOLZ_PHASE_RUN)
        return; /* no menu during the lap/banner */
    /* Only taps landing in the right quarter open the menu. */
    if (st->press_pt.x >= st->disp_w * 3 / 4)
        open_menu(st);
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
    /* Taps open the control menu; CLICKABLE is needed for press/click events. */
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(canvas, canvas_pressed_cb, LV_EVENT_PRESSED, st);
    lv_obj_add_event_cb(canvas, canvas_clicked_cb, LV_EVENT_CLICKED, st);
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
    /* A re-activation starts a fresh randomized round with no overlay open. */
    close_overlays(st);
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
    if (lv_tick_elaps(st->last_step) < speed && st->phase != GOLZ_PHASE_BANNER)
        return;

    switch (st->phase) {
    case GOLZ_PHASE_RUN: {
        golz_step(&st->game);
        golz_terminal_t t = golz_terminal(&st->game);
        render(st);
        st->last_step = lv_tick_get();
        handle_terminal(st, t);
        break;
    }
    case GOLZ_PHASE_LAP: {
        bool done = lap_step(st);
        render(st);
        st->last_step = lv_tick_get();
        if (done)
            show_banner(st); /* lap drained -> banner hold */
        break;
    }
    case GOLZ_PHASE_BANNER:
        /* A tap short-circuits via banner_clicked_cb; otherwise auto re-roll. */
        if (lv_tick_elaps(st->banner_since) >= GOLZ_BANNER_HOLD_MS)
            reseed(st);
        break;
    }
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
