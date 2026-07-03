/**
 * @file icons.c
 * Nerd Font icon browser — the "candy aisle" for ~10k glyphs.
 *
 * Layout (1920×440), claude-mode design language:
 *   [ header + 4-row glyph grid          ][ multi-size preview ][ controls ]
 *
 * The grid pages through the current set's *present* glyphs (codepoints the
 * font actually contains — sparse ranges are filtered at render time). Tapping
 * a cell selects it; the centre column shows that glyph at several sizes plus
 * its codepoint. The right column curates favourites (mark, filter to
 * favourites-only, save to disk in a bake-ready hex list).
 *
 * All logic — the set table, paging math, and the favourites set + file format
 * — lives in the pure, host-tested `iconset` core. This file is LVGL glue plus
 * the two things that need the font at runtime: glyph-presence probing and
 * rendering. Glyphs are rasterised on demand by TinyTTF (src/libs/tiny_ttf) and
 * cached; nothing is baked.
 */
#include "modes/icons.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iconset.h"
#include "lvgl.h"
#include "src/libs/tiny_ttf/lv_tiny_ttf.h"

/* Grid geometry — 4 rows is the design constraint; columns fill the left column
 * width. Cell/gap sized so 4 rows + header fit the 440px panel height. */
#define GRID_COLS    14
#define GRID_ROWS    4
#define PER_PAGE     (GRID_COLS * GRID_ROWS)
#define CELL         84
#define CELL_GAP     6
#define GRID_FONT_SZ 44

/* Preview stack: the selected glyph at these sizes so its legibility at real UI
 * sizes is obvious at a glance. */
#define PREVIEW_N 4
static const int PREVIEW_SIZES[PREVIEW_N] = {96, 64, 40, 24};

#define W_PREVIEW 360
#define W_CTRL    212
#define BTN_H     50

/* At most this many present glyphs per view. The largest single set (a Font
 * Awesome / Material Design sub-set, or the favourites list) is well under this. */
#define PRESENT_CAP 1024

#define COLOR_BG        lv_color_hex(0x05070d)
#define COLOR_PANEL     lv_color_hex(0x0a0f1a)
#define COLOR_PANEL_HI  lv_color_hex(0x101726)
#define COLOR_HAIRLINE  lv_color_hex(0x1b2334)
#define COLOR_INK       lv_color_hex(0xe9edf6)
#define COLOR_SECONDARY lv_color_hex(0x8b95ab)
#define COLOR_MUTED     lv_color_hex(0x525d73)
#define COLOR_ACCENT    lv_color_hex(0xcf6b4a) /* claude coral */
#define COLOR_OK        lv_color_hex(0x35a271)

typedef struct icons_state icons_state_t;

/* Per-cell tap context (stable storage, indexed by cell). */
typedef struct {
    icons_state_t *st;
    int            cell; /* 0..PER_PAGE-1 */
} cell_ctx_t;

struct icons_state {
    const char *ttf_path;
    const char *fav_path;

    /* The TTF bytes, loaded once and shared by every font object. TinyTTF's
     * create_data references this buffer (reads on demand) rather than copying,
     * so it must outlive the fonts — it lives for the mode's lifetime. */
    void  *ttf_data;
    size_t ttf_size;

    /* Runtime fonts (one object per simultaneously-visible size). */
    lv_font_t *grid_font;
    lv_font_t *prev_font[PREVIEW_N];
    /* A cache-less font used only for presence probing: with no glyph cache,
     * TinyTTF returns a dsc (gid.index 0 for a missing glyph) instead of logging
     * "cache not allocated" per gap. The render fonts keep their caches. */
    lv_font_t *probe_font;
    bool       fonts_ok;

    /* Model. */
    int            set_index; /* current set (when !fav_only) */
    bool           fav_only;  /* browsing the favourites-across-all-sets view */
    int            page;
    iconset_favs_t favs;
    bool           favs_loaded;

    uint32_t present[PRESENT_CAP];
    int      present_count;

    uint32_t selected_cp; /* 0 == nothing selected */

    /* Widgets. */
    lv_obj_t  *set_name;
    lv_obj_t  *set_meta;
    lv_obj_t  *cells[PER_PAGE];
    lv_obj_t  *cell_lbls[PER_PAGE];
    lv_obj_t  *cell_marks[PER_PAGE];
    cell_ctx_t cell_ctx[PER_PAGE];

    lv_obj_t *prev_lbls[PREVIEW_N];
    lv_obj_t *prev_cp;
    lv_obj_t *prev_set;
    lv_obj_t *page_lbl;

    lv_obj_t *btn_favonly_lbl;
    lv_obj_t *btn_favonly;
    lv_obj_t *btn_fav_lbl;
    lv_obj_t *toast;
};

/* ---- helpers ---------------------------------------------------------- */

/* Encode a codepoint as UTF-8 into buf (>= 5 bytes). */
static void encode_utf8(uint32_t cp, char *buf) {
    if (cp < 0x80) {
        buf[0] = (char)cp;
        buf[1] = '\0';
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        buf[2] = '\0';
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        buf[3] = '\0';
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        buf[4] = '\0';
    }
}

/* Whether the font actually contains `cp`. With the cache-less probe font,
 * get_glyph_dsc returns true even for a missing codepoint (describing .notdef,
 * glyph id 0), so presence is the resolved glyph id being non-zero — not the
 * boolean return. */
static bool glyph_present(const lv_font_t *font, uint32_t cp) {
    lv_font_glyph_dsc_t d;
    if (!lv_font_get_glyph_dsc(font, &d, cp, 0))
        return false;
    return d.gid.index != 0;
}

static int page_count(const icons_state_t *st) {
    return iconset_page_count(st->present_count, PER_PAGE);
}

/* ---- model rebuild + repaint ------------------------------------------ */

/* Recompute the present-glyph list for the current view (set or favourites),
 * then clamp the page into range. */
static void rebuild_present(icons_state_t *st) {
    st->present_count = 0;
    if (st->fav_only) {
        int n = iconset_favs_count(&st->favs);
        for (int i = 0; i < n && st->present_count < PRESENT_CAP; i++)
            st->present[st->present_count++] = iconset_favs_at(&st->favs, i);
    } else {
        iconset_set_t s;
        if (iconset_at(st->set_index, &s) == 0) {
            for (uint32_t cp = s.start;
                 cp <= s.end && st->present_count < PRESENT_CAP; cp++)
                if (glyph_present(st->probe_font, cp))
                    st->present[st->present_count++] = cp;
        }
    }
    st->page = iconset_clamp_page(st->page, st->present_count, PER_PAGE);
}

static void repaint_header(icons_state_t *st) {
    char meta[64];
    if (st->fav_only) {
        lv_label_set_text(st->set_name, "Favorites");
        snprintf(meta, sizeof(meta), "%d saved", iconset_favs_count(&st->favs));
    } else {
        iconset_set_t s;
        iconset_at(st->set_index, &s);
        lv_label_set_text(st->set_name, s.name);
        snprintf(meta, sizeof(meta), "set %d/%d  -  %d glyphs", st->set_index + 1,
                 iconset_count(), st->present_count);
    }
    lv_label_set_text(st->set_meta, meta);
}

/* Repaint one cell for present index `idx` (or hide it if past the end). */
static void paint_cell(icons_state_t *st, int i, int idx) {
    if (idx >= st->present_count) {
        lv_obj_add_flag(st->cells[i], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    uint32_t cp = st->present[idx];
    char u[5];
    encode_utf8(cp, u);
    lv_label_set_text(st->cell_lbls[i], u);
    lv_obj_clear_flag(st->cells[i], LV_OBJ_FLAG_HIDDEN);

    bool fav = iconset_favs_contains(&st->favs, cp);
    if (fav)
        lv_obj_clear_flag(st->cell_marks[i], LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(st->cell_marks[i], LV_OBJ_FLAG_HIDDEN);

    bool sel = (cp != 0 && cp == st->selected_cp);
    lv_obj_set_style_border_color(st->cells[i],
                                  sel ? COLOR_ACCENT : COLOR_HAIRLINE, 0);
    lv_obj_set_style_border_width(st->cells[i], sel ? 2 : 1, 0);
    lv_obj_set_style_bg_color(st->cells[i], sel ? COLOR_PANEL_HI : COLOR_PANEL,
                              0);
}

static void repaint_grid(icons_state_t *st) {
    int base = st->page * PER_PAGE;
    for (int i = 0; i < PER_PAGE; i++)
        paint_cell(st, i, base + i);
    char p[24];
    snprintf(p, sizeof(p), "page %d/%d", st->page + 1, page_count(st));
    lv_label_set_text(st->page_lbl, p);
    repaint_header(st);
}

static void repaint_preview(icons_state_t *st) {
    if (st->selected_cp == 0) {
        for (int k = 0; k < PREVIEW_N; k++)
            lv_label_set_text(st->prev_lbls[k], "");
        lv_label_set_text(st->prev_cp, "tap a glyph");
        lv_label_set_text(st->prev_set, "");
        return;
    }
    char u[5];
    encode_utf8(st->selected_cp, u);
    for (int k = 0; k < PREVIEW_N; k++)
        lv_label_set_text(st->prev_lbls[k], u);
    char cp[16];
    snprintf(cp, sizeof(cp), "U+%04X", st->selected_cp);
    lv_label_set_text(st->prev_cp, cp);
    const char *set = iconset_name_for_cp(st->selected_cp);
    lv_label_set_text(st->prev_set, set ? set : "");
}

/* The Favorite button reflects the selected glyph's membership. */
static void repaint_fav_btn(icons_state_t *st) {
    if (st->selected_cp == 0) {
        lv_label_set_text(st->btn_fav_lbl, "Favorite");
        lv_obj_set_style_text_color(st->btn_fav_lbl, COLOR_MUTED, 0);
    } else if (iconset_favs_contains(&st->favs, st->selected_cp)) {
        lv_label_set_text(st->btn_fav_lbl, "Unfavorite");
        lv_obj_set_style_text_color(st->btn_fav_lbl, COLOR_ACCENT, 0);
    } else {
        lv_label_set_text(st->btn_fav_lbl, "Favorite");
        lv_obj_set_style_text_color(st->btn_fav_lbl, COLOR_INK, 0);
    }
}

static void repaint_favonly_btn(icons_state_t *st) {
    lv_label_set_text(st->btn_favonly_lbl,
                      st->fav_only ? "Show All" : "Favs Only");
    lv_obj_set_style_border_color(st->btn_favonly,
                                  st->fav_only ? COLOR_ACCENT : COLOR_HAIRLINE,
                                  0);
}

static void repaint_all(icons_state_t *st) {
    rebuild_present(st);
    repaint_grid(st);
    repaint_preview(st);
    repaint_fav_btn(st);
    repaint_favonly_btn(st);
}

/* ---- favourites file I/O ---------------------------------------------- */

static void load_favs(icons_state_t *st) {
    FILE *f = fopen(st->fav_path, "rb");
    if (!f)
        return; /* missing file -> empty set, not an error */
    static char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    iconset_favs_parse(&st->favs, buf);
}

static void save_favs(icons_state_t *st) {
    static char buf[16384];
    iconset_favs_format(&st->favs, buf, sizeof(buf));
    FILE *f = fopen(st->fav_path, "wb");
    bool ok = false;
    if (f) {
        size_t len = strlen(buf);
        ok = fwrite(buf, 1, len, f) == len;
        if (fclose(f) != 0)
            ok = false;
    }
    char msg[64];
    if (ok)
        snprintf(msg, sizeof(msg), "Saved %d", iconset_favs_count(&st->favs));
    else
        snprintf(msg, sizeof(msg), "Save failed");
    lv_label_set_text(st->toast, msg);
    lv_obj_set_style_text_color(st->toast, ok ? COLOR_OK : COLOR_ACCENT, 0);
}

/* ---- event callbacks -------------------------------------------------- */

/* True if the current input is a swipe, not a tap (shell owns swipe nav). */
static bool is_gesture(void) {
    lv_indev_t *indev = lv_indev_active();
    return indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE;
}

static void cell_cb(lv_event_t *e) {
    if (is_gesture())
        return;
    cell_ctx_t *ctx = lv_event_get_user_data(e);
    icons_state_t *st = ctx->st;
    int idx = st->page * PER_PAGE + ctx->cell;
    if (idx >= st->present_count)
        return;
    st->selected_cp = st->present[idx];
    repaint_grid(st); /* move the selection highlight */
    repaint_preview(st);
    repaint_fav_btn(st);
}

/* Step the current set by delta (wrapping), leaving the favourites view. */
static void set_step(icons_state_t *st, int delta) {
    st->fav_only = false;
    st->set_index = iconset_wrap(st->set_index, delta);
    st->page = 0;
    st->selected_cp = 0;
    repaint_all(st);
}

static void setprev_cb(lv_event_t *e) {
    if (is_gesture())
        return;
    set_step(lv_event_get_user_data(e), -1);
}

static void setnext_cb(lv_event_t *e) {
    if (is_gesture())
        return;
    set_step(lv_event_get_user_data(e), +1);
}

static void favonly_cb(lv_event_t *e) {
    if (is_gesture())
        return;
    icons_state_t *st = lv_event_get_user_data(e);
    st->fav_only = !st->fav_only;
    st->page = 0;
    st->selected_cp = 0;
    repaint_all(st);
}

static void favtoggle_cb(lv_event_t *e) {
    if (is_gesture())
        return;
    icons_state_t *st = lv_event_get_user_data(e);
    if (st->selected_cp == 0)
        return;
    iconset_favs_toggle(&st->favs, st->selected_cp);
    /* In the favourites-only view an unfavourite removes it from the list. */
    if (st->fav_only)
        rebuild_present(st);
    repaint_grid(st);
    repaint_fav_btn(st);
}

static void save_cb(lv_event_t *e) {
    if (is_gesture())
        return;
    save_favs(lv_event_get_user_data(e));
}

static void prev_cb(lv_event_t *e) {
    if (is_gesture())
        return;
    icons_state_t *st = lv_event_get_user_data(e);
    st->page = iconset_clamp_page(st->page - 1, st->present_count, PER_PAGE);
    repaint_grid(st);
}

static void next_cb(lv_event_t *e) {
    if (is_gesture())
        return;
    icons_state_t *st = lv_event_get_user_data(e);
    st->page = iconset_clamp_page(st->page + 1, st->present_count, PER_PAGE);
    repaint_grid(st);
}

/* ---- screen construction ---------------------------------------------- */

static lv_obj_t *make_ctrl_btn(lv_obj_t *parent, const char *text,
                               lv_event_cb_t cb, void *user, lv_obj_t **out_lbl) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, LV_PCT(100), BTN_H);
    lv_obj_set_style_bg_color(btn, COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, COLOR_PANEL_HI, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, COLOR_HAIRLINE, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, COLOR_INK, 0);
    lv_obj_center(lbl);
    if (out_lbl)
        *out_lbl = lbl;
    return btn;
}

/* A row of two equal-width buttons (back / forward), sharing one user pointer. */
static void make_nav_row(lv_obj_t *col, const char *lt, lv_event_cb_t lcb,
                         const char *rt, lv_event_cb_t rcb, void *user) {
    lv_obj_t *nav = lv_obj_create(col);
    lv_obj_remove_style_all(nav);
    lv_obj_set_size(nav, LV_PCT(100), BTN_H);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(nav, 8, 0);
    lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(nav, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_flex_grow(make_ctrl_btn(nav, lt, lcb, user, NULL), 1);
    lv_obj_set_flex_grow(make_ctrl_btn(nav, rt, rcb, user, NULL), 1);
}

static void build_grid_column(icons_state_t *st, lv_obj_t *parent) {
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_height(col, LV_PCT(100));
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(col, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* Header: set name + meta line. */
    lv_obj_t *head = lv_obj_create(col);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, GRID_COLS * (CELL + CELL_GAP), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(head, LV_OBJ_FLAG_GESTURE_BUBBLE);

    st->set_name = lv_label_create(head);
    lv_obj_set_style_text_font(st->set_name, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(st->set_name, COLOR_ACCENT, 0);
    st->set_meta = lv_label_create(head);
    lv_obj_set_style_text_font(st->set_meta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->set_meta, COLOR_SECONDARY, 0);

    /* The wrapping cell grid. */
    lv_obj_t *grid = lv_obj_create(col);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, GRID_COLS * (CELL + CELL_GAP),
                    GRID_ROWS * (CELL + CELL_GAP));
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, CELL_GAP, 0);
    lv_obj_set_style_pad_column(grid, CELL_GAP, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_GESTURE_BUBBLE);

    for (int i = 0; i < PER_PAGE; i++) {
        lv_obj_t *cell = lv_button_create(grid);
        lv_obj_set_size(cell, CELL, CELL);
        lv_obj_set_style_bg_color(cell, COLOR_PANEL, LV_PART_MAIN);
        lv_obj_set_style_border_color(cell, COLOR_HAIRLINE, LV_PART_MAIN);
        lv_obj_set_style_border_width(cell, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(cell, 8, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(cell, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(cell, 0, LV_PART_MAIN);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_GESTURE_BUBBLE);
        st->cell_ctx[i].st = st;
        st->cell_ctx[i].cell = i;
        lv_obj_add_event_cb(cell, cell_cb, LV_EVENT_CLICKED, &st->cell_ctx[i]);

        lv_obj_t *g = lv_label_create(cell);
        /* Clear the default "Text" before applying the symbols font — that font
         * has no ASCII, so measuring the placeholder would log a missing-glyph
         * error per letter until the first repaint sets a real glyph. */
        lv_label_set_text(g, "");
        lv_obj_set_style_text_font(g, st->grid_font, 0);
        lv_obj_set_style_text_color(g, COLOR_INK, 0);
        lv_obj_center(g);

        /* Favourite marker: a small coral dot, top-right, hidden by default. */
        lv_obj_t *mark = lv_label_create(cell);
        lv_label_set_text(mark, LV_SYMBOL_OK);
        lv_obj_set_style_text_font(mark, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(mark, COLOR_ACCENT, 0);
        lv_obj_align(mark, LV_ALIGN_TOP_RIGHT, -4, 4);
        lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);

        st->cells[i] = cell;
        st->cell_lbls[i] = g;
        st->cell_marks[i] = mark;
    }
}

static void build_preview_column(icons_state_t *st, lv_obj_t *parent) {
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_set_size(col, W_PREVIEW, LV_PCT(100));
    lv_obj_set_style_bg_color(col, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_border_color(col, COLOR_HAIRLINE, 0);
    lv_obj_set_style_border_side(col, LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT,
                                 0);
    lv_obj_set_style_pad_all(col, 8, 0);
    lv_obj_set_style_pad_row(col, 4, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(col, LV_OBJ_FLAG_GESTURE_BUBBLE);

    for (int k = 0; k < PREVIEW_N; k++) {
        lv_obj_t *g = lv_label_create(col);
        lv_obj_set_style_text_font(g, st->prev_font[k], 0);
        lv_obj_set_style_text_color(g, COLOR_INK, 0);
        lv_label_set_text(g, "");
        st->prev_lbls[k] = g;
    }

    st->prev_cp = lv_label_create(col);
    lv_obj_set_style_text_font(st->prev_cp, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(st->prev_cp, COLOR_SECONDARY, 0);
    lv_obj_set_style_pad_top(st->prev_cp, 6, 0);

    st->prev_set = lv_label_create(col);
    lv_obj_set_style_text_font(st->prev_set, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->prev_set, COLOR_MUTED, 0);

    st->page_lbl = lv_label_create(col);
    lv_obj_set_style_text_font(st->page_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->page_lbl, COLOR_MUTED, 0);
}

static void build_control_column(icons_state_t *st, lv_obj_t *parent) {
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_set_size(col, W_CTRL, LV_PCT(100));
    lv_obj_set_style_bg_color(col, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 6, 0);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(col, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* Set back / forward — a two-way step so reaching a distant set is a few
     * taps in either direction, not a full lap around the ring. */
    make_nav_row(col, LV_SYMBOL_LEFT " Set", setprev_cb, "Set " LV_SYMBOL_RIGHT,
                 setnext_cb, st);
    st->btn_favonly =
        make_ctrl_btn(col, "Favs Only", favonly_cb, st, &st->btn_favonly_lbl);
    make_ctrl_btn(col, "Favorite", favtoggle_cb, st, &st->btn_fav_lbl);
    make_ctrl_btn(col, "Save", save_cb, st, NULL);
    /* Page within the current set (bare arrows — set nav is the labelled row). */
    make_nav_row(col, LV_SYMBOL_LEFT, prev_cb, LV_SYMBOL_RIGHT, next_cb, st);

    st->toast = lv_label_create(col);
    lv_obj_set_style_text_font(st->toast, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->toast, COLOR_MUTED, 0);
    lv_label_set_text(st->toast, "");
}

/* Load the whole TTF into a malloc'd buffer (plain POSIX I/O — TinyTTF's own
 * file loader would route through LVGL's lv_fs drive-letter layer, which we
 * don't register). Returns true on success, filling st->ttf_data/size. */
static bool load_ttf(icons_state_t *st) {
    FILE *f = fopen(st->ttf_path, "rb");
    if (!f)
        return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    if (sz <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    void *buf = malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return false;
    }
    st->ttf_data = buf;
    st->ttf_size = (size_t)sz;
    return true;
}

/* Fonts: one runtime TinyTTF object per simultaneously-visible size, all
 * sharing the one in-memory TTF buffer. */
static void create_fonts(icons_state_t *st) {
    if (!load_ttf(st)) {
        st->fonts_ok = false;
        return;
    }
    st->grid_font =
        lv_tiny_ttf_create_data(st->ttf_data, st->ttf_size, GRID_FONT_SZ);
    st->fonts_ok = st->grid_font != NULL;
    for (int k = 0; k < PREVIEW_N; k++) {
        st->prev_font[k] = lv_tiny_ttf_create_data(st->ttf_data, st->ttf_size,
                                                   PREVIEW_SIZES[k]);
        if (!st->prev_font[k])
            st->fonts_ok = false;
    }
    /* Cache-less probe font (cache_size 0): silent on missing glyphs. */
    st->probe_font = lv_tiny_ttf_create_data_ex(
        st->ttf_data, st->ttf_size, GRID_FONT_SZ, LV_FONT_KERNING_NONE, 0);
    if (!st->probe_font)
        st->fonts_ok = false;
}

static void build_unavailable(kd_mode_t *self, lv_obj_t *scr) {
    icons_state_t *st = self->state;
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_remove_style_all(box);
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 8, 0);

    lv_obj_t *t = lv_label_create(box);
    lv_label_set_text(t, "Nerd Font unavailable");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t, COLOR_INK, 0);
    lv_obj_t *p = lv_label_create(box);
    lv_label_set_text(p, st->ttf_path);
    lv_obj_set_style_text_font(p, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(p, COLOR_MUTED, 0);
    self->screen = scr;
}

static void build_screen(kd_mode_t *self) {
    icons_state_t *st = self->state;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    create_fonts(st);
    if (!st->fonts_ok) {
        build_unavailable(self, scr);
        return;
    }

    lv_obj_set_style_pad_all(scr, 8, 0);
    lv_obj_set_style_pad_column(scr, 6, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    build_grid_column(st, scr);
    build_preview_column(st, scr);
    build_control_column(st, scr);

    self->screen = scr;
}

static void activate(kd_mode_t *self) {
    icons_state_t *st = self->state;
    if (!self->screen)
        build_screen(self);
    if (!st->fonts_ok)
        return;
    if (!st->favs_loaded) {
        load_favs(st);
        st->favs_loaded = true;
    }
    repaint_all(st);
}

kd_mode_t *icons_mode_create(const char *id, const char *title,
                             const char *ttf_path, const char *fav_path) {
    kd_mode_t *m = calloc(1, sizeof(*m));
    icons_state_t *st = calloc(1, sizeof(*st));
    st->ttf_path = ttf_path;
    st->fav_path = fav_path;
    m->id = id;
    m->title = title;
    m->state = st;
    m->activate = activate;
    m->deactivate = NULL;
    m->tick = NULL;
    return m;
}
