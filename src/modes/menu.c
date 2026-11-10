/**
 * @file menu.c
 * Menu launcher mode: content modes are grouped into one to three side-by-side
 * section panels — "Fun" then "Ops" by default — each tile opening its mode on
 * tap. Section names, header colours and count come from KDESKDASH_MODES (korg
 * WI 668); the 18 tile slots are split evenly between the sections.
 *
 * A mode's group and order come from the **modeset** (src/modeset.h), which is
 * the same list that decided which modes got registered and in what swipe order.
 * This file used to own that mapping in static FUN_IDS/OPS_IDS tables; those
 * moved into modeset.c's roster, so per-device sets and the menu can never
 * disagree. Any registered content mode the modeset does not mention is appended
 * to the last section, so a mode can never silently vanish from the menu.
 *
 * The grids are reserved capacity, three rows tall and filled top-left: one
 * section is 6×3, two are 3×3 each (the layout since sprint 012), three are
 * 2×3 each. This replaced a single horizontal row that clipped once the modes
 * grew past ~6.
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
/* "../palette.h": src/modes/palette.h shadows the core header from in here —
 * docs/solutions/best-practices/quote-include-core-header-shadowing.md. */
#include "../palette.h"

#define TILE_W      288 /* widest a tile gets; narrowed only when it must fit */
#define TILE_H      112
#define TILE_GAP    20
#define GRID_ROWS   MODESET_MENU_ROWS
#define GROUP_CAP   MODESET_MAX_MODES /* largest a section can be (one section) */
#define SCREEN_PAD  10
#define SECTION_GAP 32 /* flex column gap: panel | divider | panel */

/* Local aliases onto the named palette (src/palette.h) — the palette is the
 * single source of truth for every color here. */
#define PAL(name) lv_color_hex(kd_pal_rgb(KD_PAL_##name))
#define COLOR_BG       PAL(VOID)
#define COLOR_PANEL    PAL(DEEP_SLATE)
#define COLOR_PANEL_HI PAL(RAISED_SLATE)  /* pressed lift */
#define COLOR_HAIRLINE PAL(GUNMETAL_SEAM)
#define COLOR_INK      PAL(MOON_INK)
#define COLOR_ACCENT   PAL(CLAUDE_CORAL)  /* claude coral */
#define COLOR_INSIDERS PAL(INSIDER_MINT)  /* VS Code Insiders green (Remote) */
#define COLOR_GOLZ     PAL(ZOMBIE_RUST)   /* dark red — zombies */

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

static void make_tile(lv_obj_t *grid, kd_mode_t *m, int tile_w) {
    lv_obj_t *tile = lv_button_create(grid);
    lv_obj_set_size(tile, tile_w, TILE_H);
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
    lv_obj_set_style_text_font(label, &kd_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, tile_text_color(m->id), LV_PART_MAIN);
    lv_obj_center(label);
}

/* The grid geometry for a given section count. Columns follow from the count
 * (18 tiles over 3 rows, split evenly); the tile width is TILE_W unless that
 * many columns, the gaps and the dividers would overflow the screen, as three
 * sections at TILE_W do by ~20px. One and two sections both get TILE_W, so the
 * two-section menu every panel shipped with is pixel-identical. */
typedef struct {
    int cols;
    int cap;
    int tile_w;
} grid_geom_t;

static grid_geom_t grid_geom(const modeset_t *ms) {
    grid_geom_t g;
    int nsec = modeset_section_count(ms);
    if (nsec < 1)
        nsec = 1;
    g.cap = modeset_section_capacity(ms);
    if (g.cap < GRID_ROWS)
        g.cap = GRID_ROWS;
    g.cols = g.cap / GRID_ROWS;

    int avail = (int)lv_display_get_horizontal_resolution(NULL) - 2 * SCREEN_PAD -
                (nsec - 1) * (2 * SECTION_GAP + 1);
    int fit = (avail / nsec - (g.cols - 1) * TILE_GAP) / g.cols;
    g.tile_w = fit < TILE_W ? fit : TILE_W;
    return g;
}

/* Build one group panel: a header over a cols×3 grid, filled top-left from the
 * `modes` list (up to g->cap; modeset_parse already warned about any extras). */
static void build_group(lv_obj_t *parent, const char *header, lv_color_t color,
                        const grid_geom_t *g, kd_mode_t *const *modes, int n) {
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
    lv_obj_set_style_text_font(lbl, &kd_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_letter_space(lbl, 4, 0);
    lv_obj_set_style_pad_left(lbl, 4, 0);

    lv_obj_t *grid = lv_obj_create(panel);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, g->cols * g->tile_w + (g->cols - 1) * TILE_GAP,
                    GRID_ROWS * TILE_H + (GRID_ROWS - 1) * TILE_GAP);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, TILE_GAP, 0);
    lv_obj_set_style_pad_column(grid, TILE_GAP, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_GESTURE_BUBBLE);

    for (int i = 0; i < n && i < g->cap; i++)
        if (modes[i])
            make_tile(grid, modes[i], g->tile_w);
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

/* The header shows a section name with its first letter capitalised, so the
 * legacy lowercase `fun`/`ops` still read "Fun"/"Ops" and an authored "Play"
 * is untouched. Only an ASCII first letter changes; UTF-8 passes through. */
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
    lv_obj_set_style_pad_all(scr, SCREEN_PAD, 0);
    lv_obj_set_style_pad_column(scr, SECTION_GAP, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    int nsec = modeset_section_count(ms);
    grid_geom_t g = grid_geom(ms);
    kd_mode_t *group[MODESET_MAX_SECTIONS][GROUP_CAP];
    int        ngroup[MODESET_MAX_SECTIONS];
    for (int s = 0; s < nsec; s++)
        ngroup[s] = collect(ms, s, group[s]);

    /* Safety net: a content mode registered without the modeset mentioning it
     * (nothing does today) joins the last section so it stays reachable. */
    if (nsec > 0) {
        int last = nsec - 1;
        int count = shell_content_count();
        for (int i = 0; i < count && ngroup[last] < g.cap; i++) {
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
        build_group(scr, header, lv_color_hex(kd_pal_rgb(modeset_section_color(ms, s))),
                    &g, group[s], ngroup[s]);
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
