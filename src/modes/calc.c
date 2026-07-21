/**
 * @file calc.c (mode)
 * Calculator mode: thin LVGL glue over the pure core in src/calc.c.
 *
 * Three zones across the 1920x440 landscape panel:
 *   left   — readouts: big result, hex/bin (when integral), and live unit
 *            conversions of the current value (in<->mm, px<->mm)
 *   middle — registers R0..R5, each with STO/RCL
 *   right  — keypad: 3x4 numpad island, binary ops, unary/constants, big "="
 *
 * Built-in Montserrat covers ASCII only, so key labels are ASCII ("x^2", "pi",
 * "/", "x") plus the LVGL backspace symbol. Every CLICKED handler carries the
 * swipe-vs-tap gesture guard (docs/solutions/best-practices/
 * lvgl-swipe-vs-tap-gesture-guard.md) and GESTURE_BUBBLE so shell navigation
 * keeps working over this button-dense screen.
 */
#include "modes/calc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Relative path: a bare "calc.h" would resolve to this directory's mode
 * header (quote-include searches the including file's dir first), silently
 * shadowing the core. */
#include "../calc.h"
#include "lvgl.h"

#define COLOR_BG        lv_color_hex(0x05070d)
#define COLOR_PANEL     lv_color_hex(0x0a0f1a)
#define COLOR_PANEL_HI  lv_color_hex(0x101726)
#define COLOR_HAIRLINE  lv_color_hex(0x1b2334)
#define COLOR_INK       lv_color_hex(0xe9edf6)
#define COLOR_SECONDARY lv_color_hex(0x8b95ab)
#define COLOR_MUTED     lv_color_hex(0x525d73)
#define COLOR_ACCENT    lv_color_hex(0xcf6b4a) /* claude coral (menu "Ops" header) */
#define COLOR_EQ        lv_color_hex(0x99492e) /* darker coral for the "=" key */
#define COLOR_TEAL      lv_color_hex(0x2ec4c4) /* Edge teal (Remote mode) */
#define COLOR_GREEN     lv_color_hex(0x38be84) /* Insiders green (Remote tile) */
#define COLOR_KEY_NUM   lv_color_hex(0x1a2332) /* digit island */
#define COLOR_KEY_OP    lv_color_hex(0x24344d) /* binary ops */
#define COLOR_KEY_FN    lv_color_hex(0x141c2b) /* unary/constants/edit */
#define COLOR_KEY_DANGER lv_color_hex(0x3a1b22) /* C */

/* Keypad geometry: 7 cols x 4 rows in an 856x424 panel. */
#define KEY_W 113
#define KEY_H 96
#define KEY_GAP 8
#define KEY_X(col) (KEY_GAP + (col) * (KEY_W + KEY_GAP))
#define KEY_Y(row) (KEY_GAP + (row) * (KEY_H + KEY_GAP))

typedef struct calc_mode_state calc_mode_state_t;

/* STO/RCL callbacks need (state, register index) in one user_data pointer. */
typedef struct {
    calc_mode_state_t *st;
    int                idx;
} reg_ref_t;

struct calc_mode_state {
    calc_t     calc;
    lv_obj_t  *result_label;
    lv_obj_t  *hex_label;
    lv_obj_t  *bin_label;
    lv_obj_t  *conv_labels[4]; /* in->mm, mm->in, mm->px, px->mm */
    lv_obj_t  *reg_labels[CALC_REGS];
    reg_ref_t  reg_refs[CALC_REGS];
};

/* --- refresh: repaint every readout from the core ------------------------- */

static void format_short(double v, char *buf, size_t n) {
    /* Conversion/register rows: 6 sig figs keeps them tidy at a glance. */
    snprintf(buf, n, "%.6g", v);
}

static void refresh(calc_mode_state_t *st) {
    char buf[64];

    calc_display(&st->calc, buf, sizeof(buf));
    lv_label_set_text(st->result_label, buf);

    lv_label_set_text(st->hex_label,
                      calc_hex(&st->calc, buf, sizeof(buf)) ? buf : "-");
    lv_label_set_text(st->bin_label,
                      calc_bin(&st->calc, buf, sizeof(buf)) ? buf : "-");

    double v = calc_value(&st->calc);
    double conv[4] = {calc_in_to_mm(v), calc_mm_to_in(v), calc_mm_to_px(v),
                      calc_px_to_mm(v)};
    for (int i = 0; i < 4; i++) {
        format_short(conv[i], buf, sizeof(buf));
        lv_label_set_text(st->conv_labels[i], buf);
    }

    for (int i = 0; i < CALC_REGS; i++) {
        if (st->calc.reg_set[i]) {
            format_short(st->calc.regs[i], buf, sizeof(buf));
            lv_label_set_text(st->reg_labels[i], buf);
            lv_obj_set_style_text_color(st->reg_labels[i], COLOR_INK,
                                        LV_PART_MAIN);
        } else {
            lv_label_set_text(st->reg_labels[i], "---");
            lv_obj_set_style_text_color(st->reg_labels[i], COLOR_MUTED,
                                        LV_PART_MAIN);
        }
    }
}

/* --- event callbacks (all gesture-guarded) -------------------------------- */

static bool is_swipe(void) {
    lv_indev_t *indev = lv_indev_active();
    return indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE;
}

static void key_cb(lv_event_t *e) {
    /* A swipe that starts on a key still releases over it; ignore those so
     * shell navigation and typing don't fight (see best-practice doc). */
    if (is_swipe())
        return;
    calc_mode_state_t *st = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    calc_key_t key = (calc_key_t)(intptr_t)lv_obj_get_user_data(btn);
    calc_key(&st->calc, key);
    refresh(st);
}

static void sto_cb(lv_event_t *e) {
    if (is_swipe())
        return;
    reg_ref_t *ref = lv_event_get_user_data(e);
    calc_store(&ref->st->calc, ref->idx);
    refresh(ref->st);
}

static void rcl_cb(lv_event_t *e) {
    if (is_swipe())
        return;
    reg_ref_t *ref = lv_event_get_user_data(e);
    calc_recall(&ref->st->calc, ref->idx);
    refresh(ref->st);
}

/* --- widget builders ------------------------------------------------------ */

static lv_obj_t *make_panel(lv_obj_t *scr, int x, int y, int w, int h) {
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_border_color(p, COLOR_HAIRLINE, LV_PART_MAIN);
    lv_obj_set_style_border_width(p, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(p, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(p, 0, LV_PART_MAIN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return p;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
    lv_obj_add_flag(l, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return l;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             const lv_font_t *font, lv_color_t bg,
                             lv_color_t fg, int x, int y, int w, int h,
                             lv_event_cb_t cb, void *user_data) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, COLOR_PANEL_HI,
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *label = make_label(btn, text, font, fg);
    lv_obj_center(label);
    return btn;
}

static void make_key(calc_mode_state_t *st, lv_obj_t *pad, const char *text,
                     calc_key_t key, const lv_font_t *font, lv_color_t bg,
                     lv_color_t fg, int col, int row, int cols, int rows) {
    lv_obj_t *btn = make_button(pad, text, font, bg, fg, KEY_X(col), KEY_Y(row),
                                KEY_W * cols + KEY_GAP * (cols - 1),
                                KEY_H * rows + KEY_GAP * (rows - 1), key_cb, st);
    lv_obj_set_user_data(btn, (void *)(intptr_t)key);
}

/* --- screen --------------------------------------------------------------- */

static void build_readouts(calc_mode_state_t *st, lv_obj_t *scr) {
    lv_obj_t *p = make_panel(scr, 12, 8, 588, 424);

    st->result_label = make_label(p, "0", &lv_font_montserrat_48, COLOR_INK);
    lv_obj_align(st->result_label, LV_ALIGN_TOP_RIGHT, -16, 14);

    /* hex/bin rows: caption left, value right, caption and value in the same
     * hue so each readout reads as one unit at a glance. */
    lv_obj_t *cap = make_label(p, "hex", &lv_font_montserrat_20, COLOR_ACCENT);
    lv_obj_set_pos(cap, 16, 92);
    st->hex_label = make_label(p, "-", &lv_font_montserrat_20, COLOR_ACCENT);
    lv_obj_align(st->hex_label, LV_ALIGN_TOP_RIGHT, -16, 88);

    cap = make_label(p, "bin", &lv_font_montserrat_20, COLOR_TEAL);
    lv_obj_set_pos(cap, 16, 128);
    st->bin_label = make_label(p, "-", &lv_font_montserrat_20, COLOR_TEAL);
    lv_obj_align(st->bin_label, LV_ALIGN_TOP_RIGHT, -16, 126);

    /* Live conversions of the current value, both directions, always on.
     * Colour keys the direction: green rows produce mm (result on the right),
     * teal rows consume mm (mm on the left, like the bin/teal family). */
    static const char *conv_caps[4] = {"in > mm", "mm > in", "mm > px",
                                       "px > mm"};
    const lv_color_t conv_colors[4] = {COLOR_GREEN, COLOR_TEAL, COLOR_TEAL,
                                       COLOR_GREEN};
    for (int i = 0; i < 4; i++) {
        int y = 178 + i * 58;
        cap = make_label(p, conv_caps[i], &lv_font_montserrat_20,
                         conv_colors[i]);
        lv_obj_set_pos(cap, 16, y + 4);
        st->conv_labels[i] =
            make_label(p, "0", &lv_font_montserrat_28, conv_colors[i]);
        lv_obj_align(st->conv_labels[i], LV_ALIGN_TOP_RIGHT, -16, y);
    }
}

static void build_registers(calc_mode_state_t *st, lv_obj_t *scr) {
    lv_obj_t *p = make_panel(scr, 612, 8, 428, 424);

    for (int i = 0; i < CALC_REGS; i++) {
        int y = 8 + i * 69;
        st->reg_refs[i].st = st;
        st->reg_refs[i].idx = i;

        char name[4];
        snprintf(name, sizeof(name), "R%d", i);
        lv_obj_t *cap =
            make_label(p, name, &lv_font_montserrat_20, COLOR_SECONDARY);
        lv_obj_set_pos(cap, 12, y + 18);

        st->reg_labels[i] = make_label(p, "---", &lv_font_montserrat_28,
                                       COLOR_MUTED);
        lv_label_set_long_mode(st->reg_labels[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(st->reg_labels[i], 172);
        lv_obj_set_pos(st->reg_labels[i], 58, y + 16);

        make_button(p, "STO", &lv_font_montserrat_20, COLOR_KEY_FN,
                    COLOR_SECONDARY, 240, y + 4, 84, 56, sto_cb,
                    &st->reg_refs[i]);
        make_button(p, "RCL", &lv_font_montserrat_20, COLOR_KEY_FN, COLOR_INK,
                    332, y + 4, 84, 56, rcl_cb, &st->reg_refs[i]);
    }
}

static void build_keypad(calc_mode_state_t *st, lv_obj_t *scr) {
    lv_obj_t *pad = make_panel(scr, 1052, 8, 856, 424);
    const lv_font_t *num = &lv_font_montserrat_36;
    const lv_font_t *fn = &lv_font_montserrat_28;

    /* col 0: unary + sign */
    make_key(st, pad, "x^2", CALC_KEY_SQR, fn, COLOR_KEY_FN, COLOR_INK, 0, 0, 1, 1);
    make_key(st, pad, "x^3", CALC_KEY_CUBE, fn, COLOR_KEY_FN, COLOR_INK, 0, 1, 1, 1);
    make_key(st, pad, "x^y", CALC_KEY_POW, fn, COLOR_KEY_FN, COLOR_INK, 0, 2, 1, 1);
    make_key(st, pad, "+/-", CALC_KEY_NEG, fn, COLOR_KEY_FN, COLOR_INK, 0, 3, 1, 1);

    /* cols 1-3: the numpad island (phone layout, muscle memory intact) */
    static const struct { const char *t; calc_key_t k; int col, row; } digits[] = {
        {"7", CALC_KEY_D7, 1, 0}, {"8", CALC_KEY_D8, 2, 0}, {"9", CALC_KEY_D9, 3, 0},
        {"4", CALC_KEY_D4, 1, 1}, {"5", CALC_KEY_D5, 2, 1}, {"6", CALC_KEY_D6, 3, 1},
        {"1", CALC_KEY_D1, 1, 2}, {"2", CALC_KEY_D2, 2, 2}, {"3", CALC_KEY_D3, 3, 2},
    };
    for (unsigned i = 0; i < sizeof(digits) / sizeof(digits[0]); i++)
        make_key(st, pad, digits[i].t, digits[i].k, num, COLOR_KEY_NUM,
                 COLOR_INK, digits[i].col, digits[i].row, 1, 1);
    make_key(st, pad, "0", CALC_KEY_D0, num, COLOR_KEY_NUM, COLOR_INK, 1, 3, 2, 1);
    make_key(st, pad, ".", CALC_KEY_DOT, num, COLOR_KEY_NUM, COLOR_INK, 3, 3, 1, 1);

    /* col 4: binary ops */
    make_key(st, pad, "/", CALC_KEY_DIV, num, COLOR_KEY_OP, COLOR_INK, 4, 0, 1, 1);
    make_key(st, pad, "x", CALC_KEY_MUL, num, COLOR_KEY_OP, COLOR_INK, 4, 1, 1, 1);
    make_key(st, pad, "-", CALC_KEY_SUB, num, COLOR_KEY_OP, COLOR_INK, 4, 2, 1, 1);
    make_key(st, pad, "+", CALC_KEY_ADD, num, COLOR_KEY_OP, COLOR_INK, 4, 3, 1, 1);

    /* cols 5-6: edit row, constants, and a big "=" */
    make_key(st, pad, LV_SYMBOL_BACKSPACE, CALC_KEY_BACKSPACE, fn, COLOR_KEY_FN,
             COLOR_INK, 5, 0, 1, 1);
    make_key(st, pad, "C", CALC_KEY_CLEAR, fn, COLOR_KEY_DANGER, COLOR_INK,
             6, 0, 1, 1);
    make_key(st, pad, "pi", CALC_KEY_PI, fn, COLOR_KEY_FN, COLOR_INK, 5, 1, 1, 1);
    make_key(st, pad, "e", CALC_KEY_E, fn, COLOR_KEY_FN, COLOR_INK, 6, 1, 1, 1);
    make_key(st, pad, "=", CALC_KEY_EQ, &lv_font_montserrat_48, COLOR_EQ,
             COLOR_INK, 5, 2, 2, 2);
}

static void build_screen(kd_mode_t *self) {
    calc_mode_state_t *st = self->state;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_readouts(st, scr);
    build_registers(st, scr);
    build_keypad(st, scr);

    self->screen = scr;
}

static void activate(kd_mode_t *self) {
    calc_mode_state_t *st = self->state;
    if (!self->screen)
        build_screen(self);
    refresh(st); /* registers/result persist across mode switches */
}

kd_mode_t *calc_mode_create(const char *id, const char *title) {
    kd_mode_t *m = calloc(1, sizeof(*m));
    calc_mode_state_t *st = calloc(1, sizeof(*st));
    calc_init(&st->calc);
    m->id = id;
    m->title = title;
    m->state = st;
    m->activate = activate;
    m->deactivate = NULL;
    m->tick = NULL; /* nothing time-based; repaint happens on key events */
    return m;
}
