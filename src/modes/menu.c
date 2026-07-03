/**
 * @file menu.c
 * Menu launcher mode: a tile per registered content mode, each opening that
 * mode on tap. The tiles are built from the shell's content-mode list, so
 * adding a content mode adds a tile with no menu-specific change.
 *
 * Visuals follow the claude-mode design language (dark surface, panel tiles
 * with a hairline border, coral accent used only for the title and the
 * pressed state). Sized so six tiles fit the 1920px panel without clipping
 * (five in use as of 2026-07); see docs/plans/2026-07-03-001.
 */
#include "modes/menu.h"

#include <stdlib.h>

#include "lvgl.h"
#include "shell.h"

#define TILE_W   280
#define TILE_H   190
#define TILE_GAP 24

#define COLOR_BG        lv_color_hex(0x05070d)
#define COLOR_PANEL     lv_color_hex(0x0a0f1a)
#define COLOR_PANEL_HI  lv_color_hex(0x101726) /* pressed lift */
#define COLOR_HAIRLINE  lv_color_hex(0x1b2334)
#define COLOR_INK       lv_color_hex(0xe9edf6)
#define COLOR_ACCENT    lv_color_hex(0xcf6b4a) /* claude coral */

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
    lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, self->title);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(title, 4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    /* A horizontal flex row of equal tiles, one per content mode. */
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), TILE_H + 8);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 14);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, TILE_GAP, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    /* Swipes that begin on the row/tiles should still bubble to the screen. */
    lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);

    int count = shell_content_count();
    for (int i = 0; i < count; i++) {
        kd_mode_t *m = shell_content_at(i);
        if (!m)
            continue;

        lv_obj_t *tile = lv_button_create(row);
        lv_obj_set_size(tile, TILE_W, TILE_H);
        lv_obj_set_style_bg_color(tile, COLOR_PANEL, LV_PART_MAIN);
        lv_obj_set_style_border_color(tile, COLOR_HAIRLINE, LV_PART_MAIN);
        lv_obj_set_style_border_width(tile, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(tile, 10, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(tile, 0, LV_PART_MAIN);
        /* The accent's one appearance: a tap answers in coral. */
        lv_obj_set_style_bg_color(tile, COLOR_PANEL_HI, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(tile, COLOR_ACCENT, LV_STATE_PRESSED);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(tile, tile_cb, LV_EVENT_CLICKED, m);

        lv_obj_t *label = lv_label_create(tile);
        lv_label_set_text(label, m->title);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_28, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, COLOR_INK, LV_PART_MAIN);
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
