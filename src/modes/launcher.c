/**
 * @file launcher.c
 * Launcher mode — see launcher.h. The mode that retires the Stream Deck.
 *
 * Layout (1920×440), 70/30:
 *   [ button grid, 1344 px ][ local + UTC clock, 576 px ]
 *
 * The split is measured, not chosen: 1344 px across three rows of ~146 px gives
 * 9 columns of ~149×146 px = ~21.6 mm cells — *larger* than a Stream Deck key
 * (19 mm). And two of the 32 Stream Deck keys were showing local and UTC time,
 * so the right-hand pane is replacing hardware, not decorating the screen.
 *
 * The mode is entirely feed-driven. It renders whatever grid
 * `kvscf:launcher:<host>` describes — including the grid *dimensions*, which
 * are on the wire precisely so the editor's idea of the layout and this
 * renderer's cannot drift — and knows nothing about URLs or Edge. Tapping a
 * button publishes `{token, button:<key>}`; kvscf resolves the destination on
 * its side, where those URLs belong.
 *
 * Model logic (parse, validate, colour resolution, label filtering) lives in
 * the pure kvscf_feed core; Redis I/O in kvscf_redis; this file is LVGL glue.
 */
#include "modes/launcher.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* "../palette.h": src/modes/palette.h shadows the core header from in here —
 * docs/solutions/best-practices/quote-include-core-header-shadowing.md. */
#include "../palette.h"
#include "clock_widget.h"
#include "kvscf_feed.h"
#include "kvscf_redis.h"
#include "lvgl.h"

#define POLL_MS      1500
#define STALE_MS     5000 /* ~3 missed polls; the key's own TTL is 10s */
#define TOAST_MS     2500
#define GRID_PCT     70
#define CLOCK_PCT    30
#define CELL_GAP     8
#define GRID_PAD     8
#define DIM_OPA      LV_OPA_40

#define PAL(name) lv_color_hex(kd_pal_rgb(KD_PAL_##name))

typedef struct lk_state lk_state_t;

typedef struct {
    lk_state_t *st;
    int         slot; /* index into cfg.buttons */
} btn_ctx_t;

struct lk_state {
    /* model */
    kvscf_launcher_t cfg;     /* last-good config — never cleared on a miss */
    kvscf_launcher_t applied; /* what the widgets currently show */
    bool             have_cfg;
    uint32_t         last_poll;
    uint32_t         last_ok;   /* tick of the last successful refresh */
    uint32_t         toast_until;
    int              last_skipped; /* warn on change, not once per poll */
    bool             token_checked;
    bool             dimmed;

    /* widgets */
    lv_obj_t          *grid;
    lv_obj_t          *buttons[KV_BUTTONS_MAX];
    lv_obj_t          *labels[KV_BUTTONS_MAX];
    btn_ctx_t          ctx[KV_BUTTONS_MAX];
    lv_obj_t          *banner; /* shown until the first config ever arrives */
    lv_obj_t          *status; /* offline / no-token / press feedback */
    kd_clock_widget_t *clock;

    /* Grid track descriptors. LVGL keeps the *pointer*, so these must outlive
     * every call to lv_obj_set_grid_dsc_array — hence state, not stack. */
    int32_t col_dsc[KV_GRID_COLS_MAX + 1];
    int32_t row_dsc[KV_GRID_ROWS_MAX + 1];
};

/* ---- label rendering: never tofu -------------------------------------- */

/* The label font is the only font on a button, deliberately. The vendored
 * Symbols Nerd Font carries no emoji at all (probed: 0 of U+1F300–1FAFF) and no
 * Latin either, so chaining it in would buy nothing — and LVGL's fallback chain
 * cannot rescue a TinyTTF primary anyway, because TinyTTF reports every glyph
 * as present. So: one bitmap font, and anything it cannot draw is filtered out
 * of the label rather than drawn as a box. See the sprint record for what it
 * would take to render Ken's emoji properly. */
#define LABEL_FONT (&lv_font_montserrat_20)

/* Glyph-presence predicate for kvscf_label_filter. Reliable here in a way it
 * would not be for TinyTTF: for LVGL's built-in LV_FONT_FMT_TXT fonts the
 * boolean return really does mean "this font has that glyph". */
static bool font_has_glyph(uint32_t cp, void *ctx) {
    lv_font_glyph_dsc_t dsc;
    return lv_font_get_glyph_dsc((const lv_font_t *)ctx, &dsc, cp, 0);
}

static bool is_gesture(void) {
    lv_indev_t *indev = lv_indev_active();
    return indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE;
}

static void set_status(lk_state_t *st, const char *msg, lv_color_t color) {
    lv_label_set_text(st->status, msg);
    lv_obj_set_style_text_color(st->status, color, 0);
}

/* The status line with no press acknowledgement on it — whatever the mode's
 * standing condition is. Every path that changes that condition routes through
 * here, so a message can never outlive the state that produced it (an earlier
 * cut un-dimmed the grid on recovery but left "kvscf offline" on screen). */
static void standing_status(lk_state_t *st) {
    if (!kvscf_redis_have_token())
        set_status(st, "no token " LV_SYMBOL_WARNING " view only", PAL(FADED_DENIM));
    else if (st->dimmed)
        set_status(st, "kvscf offline", PAL(PATIENT_AMBER));
    else
        set_status(st, "", PAL(FADED_DENIM));
}

/* ---- painting ---------------------------------------------------------- */

/* Do the two configs describe the same *layout*? `ts` moves every second and
 * must not count, or the grid would be rebuilt once a second forever. */
static bool same_layout(const kvscf_launcher_t *a, const kvscf_launcher_t *b) {
    return a->rows == b->rows && a->cols == b->cols && a->count == b->count &&
           strcmp(a->host, b->host) == 0 &&
           memcmp(a->buttons, b->buttons,
                  (size_t)a->count * sizeof(a->buttons[0])) == 0;
}

/* Rebuild the grid tracks and every button from st->cfg. Only called when the
 * layout actually changed. */
static void apply_config(lk_state_t *st) {
    const kvscf_launcher_t *c = &st->cfg;

    /* Equal fractions in both axes — the cells are near-square by arithmetic
     * (1344/9 ≈ 149 wide, 440/3 ≈ 146 tall), not by hand-tuning. */
    for (int i = 0; i < c->cols; i++)
        st->col_dsc[i] = LV_GRID_FR(1);
    st->col_dsc[c->cols] = LV_GRID_TEMPLATE_LAST;
    for (int i = 0; i < c->rows; i++)
        st->row_dsc[i] = LV_GRID_FR(1);
    st->row_dsc[c->rows] = LV_GRID_TEMPLATE_LAST;
    lv_obj_set_grid_dsc_array(st->grid, st->col_dsc, st->row_dsc);

    for (int i = 0; i < KV_BUTTONS_MAX; i++) {
        if (i >= c->count) {
            lv_obj_add_flag(st->buttons[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const kvscf_button_t *b = &c->buttons[i];
        lv_obj_t *btn = st->buttons[i];

        lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, b->col, b->w,
                             LV_GRID_ALIGN_STRETCH, b->row, b->h);

        /* An unknown or empty colour means "use the default" — never a reason
         * to drop the button. */
        uint32_t rgb;
        lv_color_t fill = kvscf_button_rgb(b->color, &rgb) ? lv_color_hex(rgb)
                                                           : PAL(DEEP_SLATE);
        lv_obj_set_style_bg_color(btn, fill, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, lv_color_lighten(fill, 40),
                                  LV_STATE_PRESSED);

        char label[KV_BTNLABEL_MAX];
        kvscf_label_filter(b->label, label, sizeof(label), font_has_glyph,
                           (void *)LABEL_FONT);
        lv_label_set_text(st->labels[i], label);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Grey the grid out without emptying it. rpidash3 sits beside a machine that
 * sleeps and locks all day; a panel that blanks every time kwork naps is worse
 * than useless, so the last-good layout stays on screen, dimmed. */
static void set_dimmed(lk_state_t *st, bool dim) {
    if (dim == st->dimmed)
        return;
    st->dimmed = dim;
    lv_obj_set_style_opa(st->grid, dim ? DIM_OPA : LV_OPA_COVER, 0);
    /* Don't stomp a fresh press acknowledgement; its decay re-reads the
     * standing condition anyway. */
    if (!st->toast_until)
        standing_status(st);
}

static void refresh(lk_state_t *st) {
    /* The token posture is static but can only be read after kvscf_redis_init,
     * which runs after shell_start — so check on the first tick, not at build. */
    if (!st->token_checked) {
        st->token_checked = true;
        standing_status(st);
    }

    if (kvscf_redis_refresh_launcher(&st->cfg)) {
        st->last_ok = lv_tick_get();
        if (!st->have_cfg) {
            st->have_cfg = true;
            lv_obj_add_flag(st->banner, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(st->grid, LV_OBJ_FLAG_HIDDEN);
        }
        /* kvscf validates before publishing, so a skip here means a genuinely
         * malformed feed. Say so — once per change, not once per poll. */
        if (st->cfg.skipped != st->last_skipped) {
            st->last_skipped = st->cfg.skipped;
            if (st->cfg.skipped > 0)
                fprintf(stderr, "kdeskdash: launcher: skipped %d invalid "
                                "button(s) from %s\n",
                        st->cfg.skipped, st->cfg.host);
        }
        if (!same_layout(&st->cfg, &st->applied)) {
            apply_config(st);
            st->applied = st->cfg;
        }
        set_dimmed(st, false);
        return;
    }

    /* No config yet: nothing to cache, so say what we are waiting for.
     *
     * The grid stays hidden here, and with it out of the flex row the clock
     * pane slides from its usual right-hand 30% over to the left. That reads
     * like a layout bug and is **not** one to fix: Ken uses the pane's position
     * as an at-a-glance "the publisher has not attached yet" signal, readable
     * across the room in a way a small grey banner is not. Restoring the 70/30
     * split in this state would take the signal away and leave the panel
     * looking identical whether or not it is receiving anything.
     *
     * If this ever does need to change, replace the signal before removing it. */
    if (!st->have_cfg) {
        lv_label_set_text(st->banner, kvscf_redis_reachable()
                                          ? "no launcher configured"
                                          : "kvscf feed unavailable");
        return;
    }
    /* One missed poll is noise; the publisher republishes about every second. */
    if (lv_tick_elaps(st->last_ok) >= STALE_MS)
        set_dimmed(st, true);
}

/* ---- events ------------------------------------------------------------ */

static void button_cb(lv_event_t *e) {
    /* A swipe that happens to release over a button must navigate, not fire —
     * see docs/solutions/best-practices/lvgl-swipe-vs-tap-gesture-guard.md. */
    if (is_gesture())
        return;
    btn_ctx_t *ctx = lv_event_get_user_data(e);
    lk_state_t *st = ctx->st;
    if (ctx->slot >= st->cfg.count)
        return;
    const kvscf_button_t *b = &st->cfg.buttons[ctx->slot];

    if (!kvscf_redis_have_token()) {
        set_status(st, "press disabled — no token", PAL(CLAUDE_CORAL));
        st->toast_until = lv_tick_get();
        return;
    }

    /* Fire-and-forget: kvscf sends no ack, so the feedback is optimistic — it
     * says "sent", not "the window came forward". */
    char msg[96];
    if (kvscf_redis_press(st->cfg.host, b->key)) {
        char label[KV_BTNLABEL_MAX];
        kvscf_label_filter(b->label, label, sizeof(label), font_has_glyph,
                           (void *)LABEL_FONT);
        snprintf(msg, sizeof(msg), LV_SYMBOL_UP " %s",
                 label[0] ? label : b->key);
        set_status(st, msg, PAL(CLAUDE_CORAL));
    } else {
        set_status(st, "press failed", PAL(ALARM_EMBER));
    }
    st->toast_until = lv_tick_get();
}

/* ---- construction ------------------------------------------------------ */

static void build_button(lk_state_t *st, int i) {
    lv_obj_t *btn = lv_button_create(st->grid);
    lv_obj_set_style_bg_color(btn, PAL(DEEP_SLATE), LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, PAL(GUNMETAL_SEAM), LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, PAL(MOON_INK), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 4, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
    /* Every child of a grid parent needs a cell from the moment it exists.
     * LVGL lays hidden children out too, and a grid child with no cell (or a
     * grid with no tracks) dereferences a null descriptor — it segfaults on the
     * first layout pass, before any feed has arrived. apply_config overwrites
     * this the moment a real config lands. */
    lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH,
                         0, 1);
    st->ctx[i].st = st;
    st->ctx[i].slot = i;
    lv_obj_add_event_cb(btn, button_cb, LV_EVENT_CLICKED, &st->ctx[i]);

    /* Wrapped and centred: "Work Items" reads as two lines on a square key,
     * the same way it did on the hardware. */
    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_font(l, LABEL_FONT, 0);
    lv_obj_set_style_text_color(l, PAL(MOON_INK), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, "");
    lv_obj_center(l);

    st->buttons[i] = btn;
    st->labels[i] = l;
}

static void build_screen(kd_mode_t *self) {
    lk_state_t *st = self->state;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, PAL(VOID), LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* Left: the grid, 70% of the width and the full height. Nothing else lives
     * in this pane — the status line went to the clock side so the measured
     * ~146 px row height survives. */
    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_PCT(GRID_PCT), LV_PCT(100));
    lv_obj_set_style_pad_all(grid, GRID_PAD, 0);
    lv_obj_set_style_pad_row(grid, CELL_GAP, 0);
    lv_obj_set_style_pad_column(grid, CELL_GAP, 0);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_HIDDEN); /* until the first config */
    st->grid = grid;

    /* A placeholder 1×1 track set, so the grid is well-formed from the start.
     * This is not an assumption about the layout — the real rows/cols come off
     * the wire in apply_config; it just keeps the first layout pass legal while
     * every button is still hidden. */
    st->col_dsc[0] = LV_GRID_FR(1);
    st->col_dsc[1] = LV_GRID_TEMPLATE_LAST;
    st->row_dsc[0] = LV_GRID_FR(1);
    st->row_dsc[1] = LV_GRID_TEMPLATE_LAST;
    lv_obj_set_grid_dsc_array(grid, st->col_dsc, st->row_dsc);

    for (int i = 0; i < KV_BUTTONS_MAX; i++)
        build_button(st, i);

    /* Right: the clock, plus the mode's one line of chrome under it. */
    lv_obj_t *side = lv_obj_create(scr);
    lv_obj_remove_style_all(side);
    lv_obj_set_size(side, LV_PCT(CLOCK_PCT), LV_PCT(100));
    lv_obj_set_style_pad_all(side, 8, 0);
    lv_obj_set_style_border_color(side, PAL(GUNMETAL_SEAM), 0);
    lv_obj_set_style_border_width(side, 1, 0);
    lv_obj_set_style_border_side(side, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(side, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(side, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(side, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *clockbox = lv_obj_create(side);
    lv_obj_remove_style_all(clockbox);
    lv_obj_set_width(clockbox, LV_PCT(100));
    lv_obj_set_flex_grow(clockbox, 1);
    lv_obj_clear_flag(clockbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(clockbox, LV_OBJ_FLAG_GESTURE_BUBBLE);

    st->status = lv_label_create(side);
    lv_obj_set_width(st->status, LV_PCT(100));
    lv_obj_set_style_text_font(st->status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->status, PAL(FADED_DENIM), 0);
    lv_obj_set_style_text_align(st->status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(st->status, LV_LABEL_LONG_DOT);
    lv_label_set_text(st->status, "");

    /* Centred over the grid pane while there is no config to draw. */
    st->banner = lv_label_create(scr);
    lv_obj_set_style_text_font(st->banner, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(st->banner, PAL(FADED_DENIM), 0);
    lv_label_set_text(st->banner, "waiting for kvscf");
    lv_obj_align(st->banner, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_width(st->banner, LV_PCT(GRID_PCT));
    lv_obj_set_style_text_align(st->banner, LV_TEXT_ALIGN_CENTER, 0);

    /* The clock measures its parent, so build it after the layout settles. */
    lv_obj_update_layout(scr);
    st->clock = kd_clock_widget_create(clockbox);

    self->screen = scr;
}

static void activate(kd_mode_t *self) {
    lk_state_t *st = self->state;
    if (!self->screen)
        build_screen(self);
    /* Refresh on the next tick, which runs after kvscf_redis_init — activate
     * can run during shell_start, before the feed handle exists. */
    st->last_poll = 0;
}

static void tick(kd_mode_t *self) {
    lk_state_t *st = self->state;
    if (!self->screen)
        return;

    kd_clock_widget_tick(st->clock);

    if (st->last_poll == 0 || lv_tick_elaps(st->last_poll) >= POLL_MS) {
        refresh(st);
        st->last_poll = lv_tick_get();
    }
    /* Let a press acknowledgement decay back to the standing status. */
    if (st->toast_until && lv_tick_elaps(st->toast_until) >= TOAST_MS) {
        st->toast_until = 0;
        standing_status(st);
    }
}

kd_mode_t *launcher_mode_create(const char *id, const char *title) {
    kd_mode_t *m = calloc(1, sizeof(*m));
    lk_state_t *st = calloc(1, sizeof(*st));
    m->id = id;
    m->title = title;
    m->state = st;
    m->activate = activate;
    m->deactivate = NULL;
    m->tick = tick;
    return m;
}
