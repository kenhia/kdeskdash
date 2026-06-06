/**
 * @file placeholder.c
 * Temporary placeholder modes used to verify shell navigation in Unit 1.
 */
#include "modes/placeholder.h"

#include <stdlib.h>

#include "lvgl.h"

typedef struct {
    uint32_t bg;
    uint32_t taps;
    lv_obj_t *tap_label;
} placeholder_state_t;

static void tap_cb(lv_event_t *e) {
    placeholder_state_t *st = lv_event_get_user_data(e);
    st->taps++;
    lv_label_set_text_fmt(st->tap_label, "taps: %u", (unsigned)st->taps);
}

static void build_screen(kd_mode_t *self) {
    placeholder_state_t *st = self->state;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(st->bg), LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, self->title);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);

    /* Tap target: a swipe that starts here must still navigate, so let the
     * gesture bubble up to the screen while taps fire CLICKED. */
    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 240, 90);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 60);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(btn, tap_cb, LV_EVENT_CLICKED, st);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "tap me");
    lv_obj_center(btn_label);

    st->tap_label = lv_label_create(scr);
    lv_label_set_text(st->tap_label, "taps: 0");
    lv_obj_set_style_text_font(st->tap_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->tap_label, lv_color_hex(0xeeeeee), LV_PART_MAIN);
    lv_obj_align(st->tap_label, LV_ALIGN_BOTTOM_MID, 0, -20);

    self->screen = scr;
}

static void activate(kd_mode_t *self) {
    if (!self->screen)
        build_screen(self);
}

kd_mode_t *placeholder_mode_create(const char *id, const char *title, uint32_t bg) {
    kd_mode_t *m = calloc(1, sizeof(*m));
    placeholder_state_t *st = calloc(1, sizeof(*st));
    st->bg = bg;
    m->id = id;
    m->title = title;
    m->state = st;
    m->activate = activate;
    m->deactivate = NULL;
    m->tick = NULL;
    return m;
}
