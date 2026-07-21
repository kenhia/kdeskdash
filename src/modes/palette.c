/**
 * @file palette.c (mode)
 * Palette mode: the living style guide. Renders the canonical named palette
 * (src/palette.h) as paged swatch cards — name and sample text in the color,
 * filled + outlined boxes, hex, and a usage note — so colors are judged on
 * the actual panel and referenced by name ("make the label CLAUDE_CORAL").
 *
 * 4x2 card grid + right paging rail (icons-mode pattern, wrapping). The mode's
 * own chrome is painted from the palette it displays. Every CLICKED handler
 * carries the swipe-vs-tap gesture guard + GESTURE_BUBBLE (best-practice doc).
 */
#include "modes/palette.h"

#include <stdio.h>
#include <stdlib.h>

/* Relative path: a bare "palette.h" would resolve to this mode header (see
 * docs/solutions/best-practices/quote-include-core-header-shadowing.md). */
#include "../palette.h"
#include "lvgl.h"

#define CARDS_PER_PAGE 8
#define CARD_W 445
#define CARD_H 207
#define CARD_GAP 10

#define PAL(id) lv_color_hex(kd_pal_rgb(KD_PAL_##id))

typedef struct {
    lv_obj_t *card; /* container; hidden when the last page runs short */
    lv_obj_t *name_label;
    lv_obj_t *usage_label;
    lv_obj_t *sample_label;
    lv_obj_t *filled_box;
    lv_obj_t *outline_box;
    lv_obj_t *hex_label;
} pal_card_t;

typedef struct {
    int        page;
    pal_card_t cards[CARDS_PER_PAGE];
    lv_obj_t  *page_label;
} palette_mode_state_t;

static int page_count(void) {
    return (kd_pal_count() + CARDS_PER_PAGE - 1) / CARDS_PER_PAGE;
}

static void refresh_page(palette_mode_state_t *st) {
    char buf[32];
    for (int slot = 0; slot < CARDS_PER_PAGE; slot++) {
        pal_card_t *c = &st->cards[slot];
        int idx = st->page * CARDS_PER_PAGE + slot;
        if (idx >= kd_pal_count()) {
            lv_obj_add_flag(c->card, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(c->card, LV_OBJ_FLAG_HIDDEN);

        lv_color_t color = lv_color_hex(kd_pal_rgb(idx));
        lv_label_set_text(c->name_label, kd_pal_name(idx));
        lv_obj_set_style_text_color(c->name_label, color, LV_PART_MAIN);
        lv_label_set_text(c->usage_label, kd_pal_usage(idx));
        lv_obj_set_style_text_color(c->sample_label, color, LV_PART_MAIN);
        lv_obj_set_style_bg_color(c->filled_box, color, LV_PART_MAIN);
        lv_obj_set_style_border_color(c->outline_box, color, LV_PART_MAIN);
        snprintf(buf, sizeof(buf), "#%06x", kd_pal_rgb(idx));
        lv_label_set_text(c->hex_label, buf);
    }
    snprintf(buf, sizeof(buf), "%d/%d", st->page + 1, page_count());
    lv_label_set_text(st->page_label, buf);
}

static void turn_page(lv_event_t *e, int dir) {
    /* Swipe-vs-tap guard: a shell swipe releasing over the button is not a
     * tap (see lvgl-swipe-vs-tap-gesture-guard.md). */
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE)
        return;
    palette_mode_state_t *st = lv_event_get_user_data(e);
    int n = page_count();
    st->page = (st->page + dir + n) % n;
    refresh_page(st);
}

static void prev_cb(lv_event_t *e) { turn_page(e, -1); }
static void next_cb(lv_event_t *e) { turn_page(e, +1); }

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
    lv_obj_add_flag(l, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return l;
}

static lv_obj_t *make_box(lv_obj_t *parent, int x, int y) {
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, 56, 36);
    lv_obj_set_style_radius(b, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return b;
}

static void build_card(palette_mode_state_t *st, lv_obj_t *scr, int slot) {
    pal_card_t *c = &st->cards[slot];
    int col = slot % 4, row = slot / 4;

    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_pos(card, 8 + col * (CARD_W + CARD_GAP),
                   8 + row * (CARD_H + CARD_GAP));
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_set_style_bg_color(card, PAL(DEEP_SLATE), LV_PART_MAIN);
    lv_obj_set_style_border_color(card, PAL(GUNMETAL_SEAM), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    c->card = card;

    c->name_label = make_label(card, "", &lv_font_montserrat_28, PAL(MOON_INK));
    lv_obj_set_pos(c->name_label, 14, 10);

    c->usage_label =
        make_label(card, "", &lv_font_montserrat_14, PAL(STEEL_MIST));
    lv_label_set_long_mode(c->usage_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(c->usage_label, CARD_W - 28);
    lv_obj_set_pos(c->usage_label, 14, 52);

    c->sample_label = make_label(card, "Handgloves 0123456789",
                                 &lv_font_montserrat_20, PAL(MOON_INK));
    lv_obj_set_pos(c->sample_label, 14, 84);

    c->filled_box = make_box(card, 14, 148);
    lv_obj_set_style_border_color(c->filled_box, PAL(GUNMETAL_SEAM),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(c->filled_box, 1, LV_PART_MAIN);

    c->outline_box = make_box(card, 82, 148);
    lv_obj_set_style_bg_opa(c->outline_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(c->outline_box, 3, LV_PART_MAIN);

    c->hex_label =
        make_label(card, "", &lv_font_montserrat_20, PAL(STEEL_MIST));
    lv_obj_set_pos(c->hex_label, 156, 156);
}

static void build_rail(palette_mode_state_t *st, lv_obj_t *scr) {
    const int x = 1828, w = 84;

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_pos(btn, x, 8);
    lv_obj_set_size(btn, w, 140);
    lv_obj_set_style_bg_color(btn, PAL(RAISED_SLATE), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(btn, prev_cb, LV_EVENT_CLICKED, st);
    lv_obj_t *l = make_label(btn, LV_SYMBOL_UP, &lv_font_montserrat_28,
                             PAL(MOON_INK));
    lv_obj_center(l);

    st->page_label =
        make_label(scr, "1/1", &lv_font_montserrat_20, PAL(STEEL_MIST));
    lv_obj_set_pos(st->page_label, x + 18, 208);

    btn = lv_button_create(scr);
    lv_obj_set_pos(btn, x, 292);
    lv_obj_set_size(btn, w, 140);
    lv_obj_set_style_bg_color(btn, PAL(RAISED_SLATE), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(btn, next_cb, LV_EVENT_CLICKED, st);
    l = make_label(btn, LV_SYMBOL_DOWN, &lv_font_montserrat_28, PAL(MOON_INK));
    lv_obj_center(l);
}

static void build_screen(kd_mode_t *self) {
    palette_mode_state_t *st = self->state;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, PAL(VOID), LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    for (int slot = 0; slot < CARDS_PER_PAGE; slot++)
        build_card(st, scr, slot);
    build_rail(st, scr);

    self->screen = scr;
    refresh_page(st);
}

static void activate(kd_mode_t *self) {
    if (!self->screen)
        build_screen(self);
}

kd_mode_t *palette_mode_create(const char *id, const char *title) {
    kd_mode_t *m = calloc(1, sizeof(*m));
    palette_mode_state_t *st = calloc(1, sizeof(*st));
    m->id = id;
    m->title = title;
    m->state = st;
    m->activate = activate;
    m->deactivate = NULL;
    m->tick = NULL; /* static content; repaint only on page turns */
    return m;
}
