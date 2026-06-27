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
#include <string.h>

#include "golz.h"
#include "lvgl.h"
#include "redis.h"

/* How long the end-of-game banner holds before auto re-roll (a tap
 * short-circuits it). */
#define GOLZ_BANNER_HOLD_MS 12000

/* Per-round machete knob ranges, chosen by the monte-carlo balance sweep
 * (tools/golz_mc.c; see docs/plans/2026-06-27-001-feat-golz-machetes-plan.md).
 * Inclusive [min,max], drawn uniformly each round. Paired with the symmetric
 * +3/-3 steps below, these land the self-balanced equilibrium threshold near the
 * 250 default (so it barely drifts) while keeping the tie rate down (~59%); the
 * symmetric steps hold the decisive split at ~50/50. */
#define GOLZ_MACHETE_PCT_MIN 8
#define GOLZ_MACHETE_PCT_MAX 18
#define GOLZ_HUMAN_KILL_MIN 18
#define GOLZ_HUMAN_KILL_MAX 32

/* Adaptive human-win threshold: starting value and self-balancing steps. The
 * floor (100) is enforced by redis_golz_set_gens_to_win. Symmetric steps give a
 * fixed point where P(Human win) == P(Zombie win), i.e. a true 50/50 decisive
 * match (the loop self-tunes the threshold to hold it). */
#define GOLZ_GENS_TO_WIN_DEFAULT 250
#define GOLZ_GENS_WIN_STEP 3  /* threshold rises after a Human win */
#define GOLZ_GENS_LOSS_STEP 3 /* threshold falls after a Zombie win */

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

    /* In-memory mirrors of the persistent post-machete counters. */
    long human_wins;
    long zombie_wins;
    long ties;
    long gens_to_win;             /* adaptive human-win threshold (floor 100) */
    long historical_zombie_wins;  /* legacy pre-machete zombie wins (display only) */
    bool outcome_recorded;        /* per-round latch: counted once */
    golz_terminal_t outcome;      /* which terminal ended the round (for the banner) */

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
    /* GoLZ-specific, drawn last so additions never shift earlier streams. */
    z->initial_count = (int)(gol_rand_u32(rng) % 3);             /* 0..2 */
    z->zombie_reinfect = (int)(gol_rand_u32(rng) % 101);         /* 0..100 % */
    z->zombie_spawn_chance = (int)(gol_rand_u32(rng) % 41);      /* 0..40 % */
    z->max_generations = 2000 + (int)(gol_rand_u32(rng) % 2000); /* 2000..3999 */
    /* Machetes, drawn last of all. generations_to_win is NOT rolled here — it is
     * the adaptive Redis-backed threshold, set by the caller after this returns. */
    z->machete_percentage =
        GOLZ_MACHETE_PCT_MIN +
        (int)(gol_rand_u32(rng) % (GOLZ_MACHETE_PCT_MAX - GOLZ_MACHETE_PCT_MIN + 1));
    z->human_kill_zombie =
        GOLZ_HUMAN_KILL_MIN +
        (int)(gol_rand_u32(rng) % (GOLZ_HUMAN_KILL_MAX - GOLZ_HUMAN_KILL_MIN + 1));
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

/* Format a non-negative count with thousands separators into buf (e.g. 13883 ->
 * "13,883"). buf must hold at least 32 bytes. */
static void format_commas(long v, char *buf, size_t buflen) {
    char raw[24];
    snprintf(raw, sizeof(raw), "%ld", v < 0 ? 0 : v);
    int len = (int)strlen(raw);
    int out = 0;
    for (int i = 0; i < len && out < (int)buflen - 1; i++) {
        if (i > 0 && (len - i) % 3 == 0 && out < (int)buflen - 1)
            buf[out++] = ',';
        buf[out++] = raw[i];
    }
    buf[out] = '\0';
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
    st->outcome_recorded = false;
    st->outcome = GOLZ_CONTINUE;
    st->last_step = lv_tick_get();
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
    /* The human-win threshold is the adaptive, persisted value (not rolled). */
    z.generations_to_win = (int)st->gens_to_win;
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

static void show_banner(golz_mode_state_t *st); /* defined below */

/* Record one decisive round outcome exactly once (latched): bump the matching
 * persistent counter (preferring the authoritative Redis value, falling back to
 * the in-memory mirror), and for a Human/Zombie win nudge the adaptive
 * generations_to_win (the loser gets an easier next game) and persist it. */
static void record_outcome(golz_mode_state_t *st, golz_terminal_t outcome) {
    if (st->outcome_recorded)
        return;
    st->outcome = outcome;
    switch (outcome) {
    case GOLZ_HUMAN_WIN: {
        long n = redis_golz_incr_human_wins();
        st->human_wins = (n >= 0) ? n : st->human_wins + 1;
        st->gens_to_win += GOLZ_GENS_WIN_STEP; /* humans won -> harder next time */
        st->gens_to_win = redis_golz_set_gens_to_win(st->gens_to_win);
        break;
    }
    case GOLZ_ZOMBIE_WIN: {
        long n = redis_golz_incr_zombie_wins();
        st->zombie_wins = (n >= 0) ? n : st->zombie_wins + 1;
        st->gens_to_win -= GOLZ_GENS_LOSS_STEP; /* humans lost -> easier next time */
        st->gens_to_win = redis_golz_set_gens_to_win(st->gens_to_win);
        break;
    }
    case GOLZ_TIE: {
        long n = redis_golz_incr_ties();
        st->ties = (n >= 0) ? n : st->ties + 1;
        break; /* a tie leaves the threshold unchanged */
    }
    default:
        return; /* CONTINUE is not a decisive outcome */
    }
    st->outcome_recorded = true;
}

/* Route the per-generation terminal decision while running. */
static void handle_terminal(golz_mode_state_t *st, golz_terminal_t t) {
    switch (t) {
    case GOLZ_ZOMBIE_WIN:
        record_outcome(st, t);
        st->phase = GOLZ_PHASE_LAP; /* victory lap, then the banner (R20) */
        st->last_step = lv_tick_get();
        break;
    case GOLZ_HUMAN_WIN:
    case GOLZ_TIE:
        record_outcome(st, t);
        show_banner(st); /* no lap: straight to the end-of-game banner */
        break;
    default:
        break; /* GOLZ_CONTINUE: keep running */
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

/* Show the full-screen end-of-game banner over the frozen board: who won, the
 * running Humans/Zombies/Ties tally, the next round's survival threshold, and
 * (smaller) the historical pre-machete zombie wins. */
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

    /* Outcome-specific title + accent colour (humans blue, zombies red, tie grey). */
    const char *title;
    uint32_t accent;
    switch (st->outcome) {
    case GOLZ_HUMAN_WIN:
        title = "The Humans Won!";
        accent = 0x4d9dff; /* blue, matching the machete cells */
        break;
    case GOLZ_ZOMBIE_WIN:
        title = "The Zombies Won";
        accent = 0xff4d4d; /* red */
        break;
    default:
        title = "Stalemate - Tie";
        accent = 0xb8c0cc; /* grey */
        break;
    }

    /* Main block: title + the three running totals + next-round threshold. */
    char buf[160];
    snprintf(buf, sizeof(buf),
             "%s\nHumans %ld   Zombies %ld   Ties %ld\nNext: Humans must last %ld "
             "generations",
             title, st->human_wins, st->zombie_wins, st->ties, st->gens_to_win);
    lv_obj_t *lbl = lv_label_create(scrim);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(accent), LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -14);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* Smaller historical footnote: pre-machete zombie wins. */
    char hist[64], num[32];
    format_commas(st->historical_zombie_wins, num, sizeof(num));
    snprintf(hist, sizeof(hist), "Historical zombie wins: %s", num);
    lv_obj_t *foot = lv_label_create(scrim);
    lv_label_set_text(foot, hist);
    lv_obj_set_style_text_font(foot, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(foot, lv_color_hex(0x8a929e), LV_PART_MAIN);
    lv_obj_set_style_text_align(foot, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_flag(foot, LV_OBJ_FLAG_GESTURE_BUBBLE);

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

    self->screen = scr;
}

static void activate(kd_mode_t *self) {
    golz_mode_state_t *st = self->state;
    if (!self->screen)
        build_screen(self);
    /* A re-activation starts a fresh randomized round with no overlay open. */
    close_overlays(st);
    /* Restore the persisted counters and adaptive threshold into the in-memory
     * mirrors (defaults when Redis is absent), then start a fresh round. The
     * historical zombie-win figure is the legacy pre-machete counter; default to
     * the documented baseline (13,883) when the key is absent. */
    st->human_wins = redis_golz_get_human_wins(0);
    st->zombie_wins = redis_golz_get_zombie_wins(0);
    st->ties = redis_golz_get_ties(0);
    st->gens_to_win = redis_golz_get_gens_to_win(GOLZ_GENS_TO_WIN_DEFAULT);
    st->historical_zombie_wins = redis_golz_get_wins(13883);
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
