/**
 * @file menu.c
 * Menu launcher mode: content modes are grouped into side-by-side 3×3 panels —
 * "Fun" then "Ops" today — each tile opening its mode on tap.
 *
 * A mode's group and order come from the **modeset** (src/modeset.h), which is
 * the same list that decided which modes got registered and in what swipe order.
 * This file used to own that mapping in static FUN_IDS/OPS_IDS tables; those
 * moved into modeset.c's roster, so per-device sets and the menu can never
 * disagree. Any registered content mode the modeset does not mention is appended
 * to the last section, so a mode can never silently vanish from the menu.
 *
 * The 3×3 grids are reserved capacity (9 slots/side, filled top-left); this
 * replaced a single horizontal row that clipped once the modes grew past ~6.
 * Visuals follow the claude-mode design language (dark panel tiles, hairline
 * border, coral pressed accent). See sprints/012-menu-groups.md.
 */
#include "modes/menu.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "shell.h"

#define TILE_W    288
#define TILE_H    112
#define TILE_GAP  20
#define GRID_COLS 3
#define GRID_ROWS 3
#define GROUP_CAP (GRID_COLS * GRID_ROWS)

#define COLOR_BG        lv_color_hex(0x05070d)
#define COLOR_PANEL     lv_color_hex(0x0a0f1a)
#define COLOR_PANEL_HI  lv_color_hex(0x101726) /* pressed lift */
#define COLOR_HAIRLINE  lv_color_hex(0x1b2334)
#define COLOR_INK       lv_color_hex(0xe9edf6)
#define COLOR_ACCENT    lv_color_hex(0xcf6b4a) /* claude coral */
#define COLOR_INSIDERS  lv_color_hex(0x38be84) /* VS Code Insiders green (Remote) */
#define COLOR_GOLZ      lv_color_hex(0xc0392b) /* dark red — zombies */

/* Per-mode tile-text colour (default ink). Claude reuses the coral accent; Remote
 * the Insiders green from the foreground mode; GoLZ a dark red. GoL/Icons/Clock/
 * Dev stay ink for now. */
static lv_color_t tile_text_color(const char *id) {
    if (id) {
        if (strcmp(id, "claude") == 0)
            return COLOR_ACCENT;
        if (strcmp(id, "foreground") == 0)
            return COLOR_INSIDERS;
        if (strcmp(id, "golz") == 0)
            return COLOR_GOLZ;
    }
    return COLOR_INK;
}

/* The modeset the menu draws. Borrowed from main.c (static there), or the
 * built-in default when the caller passes NULL. */
typedef struct {
    const modeset_t *modes;
    modeset_t        fallback;
} menu_state_t;

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

static void make_tile(lv_obj_t *grid, kd_mode_t *m) {
    lv_obj_t *tile = lv_button_create(grid);
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
    lv_obj_set_style_text_color(label, tile_text_color(m->id), LV_PART_MAIN);
    lv_obj_center(label);
}

/* Build one group panel: a header over a 3×3 grid, filled top-left from the
 * `modes` list (up to GROUP_CAP; extras beyond 9 are dropped — revisit then). */
static void build_group(lv_obj_t *parent, const char *header,
                        kd_mode_t *const *modes, int n) {
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *lbl = lv_label_create(panel);
    lv_label_set_text(lbl, header);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, COLOR_ACCENT, 0);
    lv_obj_set_style_text_letter_space(lbl, 4, 0);
    lv_obj_set_style_pad_left(lbl, 4, 0);

    lv_obj_t *grid = lv_obj_create(panel);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, GRID_COLS * TILE_W + (GRID_COLS - 1) * TILE_GAP,
                    GRID_ROWS * TILE_H + (GRID_ROWS - 1) * TILE_GAP);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, TILE_GAP, 0);
    lv_obj_set_style_pad_column(grid, TILE_GAP, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_GESTURE_BUBBLE);

    for (int i = 0; i < n && i < GROUP_CAP; i++)
        if (modes[i])
            make_tile(grid, modes[i]);
}

/* Collect one section's registered modes, in the modeset's order, skipping any
 * that never registered. Returns the count placed in out (capacity GROUP_CAP). */
static int collect(const modeset_t *ms, int s, kd_mode_t **out) {
    int n = 0;
    int size = modeset_section_size(ms, s);
    for (int i = 0; i < size && n < GROUP_CAP; i++) {
        kd_mode_t *m = shell_find_mode(modeset_section_at(ms, s, i));
        if (m)
            out[n++] = m;
    }
    return n;
}

/* Section names are lowercase ids ("fun"); the header shows them title-cased.
 * When section names become author-supplied (korg WI 668) this is the seam that
 * stops doing the capitalising and starts trusting the name verbatim. */
static void header_text(char *dst, size_t cap, const char *name) {
    snprintf(dst, cap, "%s", name ? name : "");
    if (dst[0])
        dst[0] = (char)toupper((unsigned char)dst[0]);
}

/* Thin vertical rule between two section panels. */
static void build_divider(lv_obj_t *parent) {
    lv_obj_t *div = lv_obj_create(parent);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 1, LV_PCT(80));
    lv_obj_set_style_bg_color(div, COLOR_HAIRLINE, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_add_flag(div, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static void build_screen(kd_mode_t *self) {
    menu_state_t *st = self->state;
    const modeset_t *ms = st->modes;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 10, 0);
    lv_obj_set_style_pad_column(scr, 32, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    int nsec = modeset_section_count(ms);
    kd_mode_t *group[MODESET_MAX_SECTIONS][GROUP_CAP];
    int        ngroup[MODESET_MAX_SECTIONS];
    for (int s = 0; s < nsec; s++)
        ngroup[s] = collect(ms, s, group[s]);

    /* Safety net: a content mode registered without the modeset mentioning it
     * (nothing does today) joins the last section so it stays reachable. */
    if (nsec > 0) {
        int last = nsec - 1;
        int count = shell_content_count();
        for (int i = 0; i < count && ngroup[last] < GROUP_CAP; i++) {
            kd_mode_t *m = shell_content_at(i);
            if (m && m->id && !modeset_section_of(ms, m->id))
                group[last][ngroup[last]++] = m;
        }
    }

    for (int s = 0; s < nsec; s++) {
        char header[MODESET_NAME_MAX];
        header_text(header, sizeof(header), modeset_section_name(ms, s));
        if (s > 0)
            build_divider(scr);
        build_group(scr, header, group[s], ngroup[s]);
    }

    self->screen = scr;
}

static void activate(kd_mode_t *self) {
    /* Build lazily so all content modes are already registered. */
    if (!self->screen)
        build_screen(self);
}

kd_mode_t *menu_mode_create(const char *id, const char *title,
                            const modeset_t *modes) {
    kd_mode_t *m = calloc(1, sizeof(*m));
    menu_state_t *st = calloc(1, sizeof(*st));

    /* A NULL modeset would otherwise mean an empty menu — draw the full default
     * instead, matching the modeset core's own "never blank the panel" rule. */
    if (modes) {
        st->modes = modes;
    } else {
        modeset_default(&st->fallback);
        st->modes = &st->fallback;
    }

    m->id = id;
    m->title = title;
    m->state = st;
    m->activate = activate;
    m->deactivate = NULL;
    m->tick = NULL;
    return m;
}
