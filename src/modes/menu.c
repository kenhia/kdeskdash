/**
 * @file menu.c
 * Menu launcher mode: a tile per registered content mode, each opening that
 * mode on tap. The tiles are built from the shell's content-mode list, so
 * adding a content mode adds a tile with no menu-specific change.
 */
#include "modes/menu.h"

#include <stdlib.h>

#include "lvgl.h"
#include "shell.h"

static void tile_cb(lv_event_t *e) {
    /* A swipe that starts on a tile still releases over it, so LVGL would fire
     * CLICKED here. If the touch was actually a gesture (e.g. swipe-down to the
     * menu), ignore it so only genuine taps launch a mode. */
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE)
        return;

    kd_mode_t *target = lv_event_get_user_data(e);
    if (target)
        shell_set_active(target);
}

static void build_screen(kd_mode_t *self) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1d24), LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, self->title);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0x8a93a6), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    /* A horizontal flex row of equal tiles, one per content mode. */
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 320);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 40, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    /* Swipes that begin on the row/tiles should still bubble to the screen. */
    lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);

    int count = shell_content_count();
    for (int i = 0; i < count; i++) {
        kd_mode_t *m = shell_content_at(i);
        if (!m)
            continue;

        lv_obj_t *tile = lv_button_create(row);
        lv_obj_set_size(tile, 360, 260);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x2b3340), LV_PART_MAIN);
        lv_obj_set_style_radius(tile, 16, LV_PART_MAIN);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(tile, tile_cb, LV_EVENT_CLICKED, m);

        lv_obj_t *label = lv_label_create(tile);
        lv_label_set_text(label, m->title);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_36, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(0xeaf0fb), LV_PART_MAIN);
        lv_obj_center(label);
    }

    self->screen = scr;
}

static void activate(kd_mode_t *self) {
    /* Build lazily so all content modes are already registered. */
    if (!self->screen)
        build_screen(self);
}

kd_mode_t *menu_mode_create(const char *id, const char *title) {
    kd_mode_t *m = calloc(1, sizeof(*m));
    m->id = id;
    m->title = title;
    m->state = NULL;
    m->activate = activate;
    m->deactivate = NULL;
    m->tick = NULL;
    return m;
}
