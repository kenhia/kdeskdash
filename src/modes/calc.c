/**
 * @file calc.c (mode)
 * Calculator mode: thin LVGL glue over the pure core in src/calc.c.
 *
 * Three zones across the 1920x440 landscape panel:
 *   left   — readouts: big result, hex/bin (when integral), and live unit
 *            conversions of the current value (in<->mm, px<->mm)
 *   middle — registers R0..R5: tap a row to recall, hold it to store
 *   right  — keypad: 3x4 numpad island, binary ops, unary/constants, trig,
 *            and a big "="
 *
 * Built-in Montserrat covers ASCII only, so key labels are ASCII ("x^2", "pi",
 * "sqrt", "1/x", "sin") plus the LVGL backspace symbol. Every handler carries
 * the swipe-vs-tap gesture guard (docs/solutions/best-practices/
 * lvgl-swipe-vs-tap-gesture-guard.md) and GESTURE_BUBBLE so shell navigation
 * keeps working over this button-dense screen.
 *
 * ## Where the trig keys came from (sprint 036, WI #509)
 *
 * The 7x4 keypad was **full** — 28 cells, 28 occupied — so the eight new keys
 * (sin cos tan INV sqrt 1/x DEG CE) had to come from somewhere. They came from
 * the register column: replacing its twelve STO/RCL buttons with tap-to-recall
 * and hold-to-store narrows that panel from 428 px to 240 px, which buys the
 * keypad two more columns at **112 px** — within a pixel of the 113 px keys it
 * had before. Nothing shrank; the keypad grew into space the register buttons
 * were spending on chrome.
 *
 * Six registers, not eight, and that is measured rather than assumed: the row
 * is the touch target now, and six rows in the 424 px panel are 60 px (7.8 mm)
 * tall. Eight would be 45 px — 5.9 mm — which is too short to hold reliably,
 * and a hold that misses silently recalls instead of storing.
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
#include "panel_store.h"
/* "../palette.h": src/modes/palette.h shadows the core header from in here —
 * docs/solutions/best-practices/quote-include-core-header-shadowing.md. */
#include "../palette.h"

/* Local aliases onto the named palette (src/palette.h) — the palette is the
 * single source of truth for every color here. */
#define PAL(name) lv_color_hex(kd_pal_rgb(KD_PAL_##name))
#define COLOR_BG         PAL(VOID)
#define COLOR_PANEL      PAL(DEEP_SLATE)
#define COLOR_PANEL_HI   PAL(RAISED_SLATE)
#define COLOR_HAIRLINE   PAL(GUNMETAL_SEAM)
#define COLOR_INK        PAL(MOON_INK)
#define COLOR_SECONDARY  PAL(STEEL_MIST)
#define COLOR_MUTED      PAL(FADED_DENIM)
#define COLOR_ACCENT     PAL(CLAUDE_CORAL)  /* claude coral (menu "Ops" header) */
#define COLOR_EQ         PAL(BURNT_CORAL)   /* darker coral for the "=" key */
#define COLOR_TEAL       PAL(EDGE_TEAL)     /* Edge teal (Remote mode) */
#define COLOR_GREEN      PAL(INSIDER_MINT)  /* Insiders green (Remote tile) */
#define COLOR_KEY_NUM    PAL(SLATE_KEY)     /* digit island */
#define COLOR_KEY_OP     PAL(STEEL_KEY)     /* binary ops */
#define COLOR_KEY_FN     PAL(QUIET_KEY)     /* unary/constants/edit */
#define COLOR_KEY_DANGER PAL(SMOKED_MAROON) /* C */

/* Zone geometry. The three panels and their gutters tile the 1920 px width
 * exactly: 12 + 540 | 12 | 240 | 12 | 1092 + 12. */
#define ZONE_Y 8
#define ZONE_H 424
#define READOUT_X 12
#define READOUT_W 540
#define REG_X 564
#define REG_W 240
#define PAD_X 816
#define PAD_W 1092

/* Keypad geometry: 9 cols x 4 rows in a 1092x424 panel.
 * 2*KEY_GAP + 9*KEY_W + 8*KEY_GAP = 16 + 1008 + 64 = 1088, four spare. */
#define KEY_W 112
#define KEY_H 96
#define KEY_GAP 8
#define KEY_X(col) (KEY_GAP + (col) * (KEY_W + KEY_GAP))
#define KEY_Y(row) (KEY_GAP + (row) * (KEY_H + KEY_GAP))

/* Register rows: six 60 px rows plus the interaction hint beneath them. */
#define REG_ROW_H 60
#define REG_ROW_PITCH 64
#define REG_ROW_Y(i) (6 + (i) * REG_ROW_PITCH)

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
    lv_obj_t  *drg_label; /* the DEG/RAD key's own label — it names the mode */
    lv_obj_t  *inv_btn;   /* the INV key, lit while the modifier is armed */
    /* The registers have been reconciled with the store: either a stored line
     * was read back, or the user stored one here. Until then a later activate
     * may still try the read — the state file is optional and may simply have
     * been unreadable at boot. Once true it never flips back, so a
     * restored-or-local register file can never be overwritten by a stale read. */
    bool       regs_loaded;
};

/* --- register persistence (WI #509) --------------------------------------
 *
 * The contract is pure and host-tested (calc_regs_serialize / calc_regs_parse);
 * this is just the I/O around it. Both directions are no-ops when the state
 * file cannot be written, which leaves the registers behaving exactly as they
 * did before they were persisted at all. (They lived on the board's own Redis
 * until sprint 039; the store is the file now, and the API is the same shape.) */

static void load_regs(calc_mode_state_t *st) {
    if (st->regs_loaded)
        return;
    char line[CALC_REGS_STR_MAX];
    if (panel_store_get_calc_regs(line, sizeof(line)) &&
        calc_regs_parse(&st->calc, line))
        st->regs_loaded = true;
}

static void save_regs(calc_mode_state_t *st) {
    /* Mark first: the user has made local state, so a later load must not
     * reach past it even if this write does not land. */
    st->regs_loaded = true;
    char line[CALC_REGS_STR_MAX];
    calc_regs_serialize(&st->calc, line, sizeof(line));
    panel_store_set_calc_regs(line);
}

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

    /* Two keys double as the readout for their own state — the angle mode has
     * no other indicator, and an armed INV that looks like an idle INV turns
     * the next trig key into a coin toss. */
    if (st->drg_label)
        lv_label_set_text(st->drg_label, st->calc.degrees ? "DEG" : "RAD");
    if (st->inv_btn)
        lv_obj_set_style_bg_color(st->inv_btn,
                                  st->calc.inv ? COLOR_ACCENT : COLOR_KEY_FN,
                                  LV_PART_MAIN);
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

/* Hold a register row to store into it.
 *
 * LV_EVENT_LONG_PRESSED fires mid-press, before release — and that is fine for
 * the swipe guard, because LVGL evaluates the gesture (indev_gesture) earlier
 * in the same press iteration than it checks the long-press timer. A swipe
 * that happens to linger over a row is therefore already a gesture by the time
 * this could fire. */
static void reg_hold_cb(lv_event_t *e) {
    if (is_swipe())
        return;
    reg_ref_t *ref = lv_event_get_user_data(e);
    calc_store(&ref->st->calc, ref->idx);
    save_regs(ref->st);
    refresh(ref->st);
}

/* Tap a register row to recall it.
 *
 * **SHORT_CLICKED, not CLICKED.** LVGL sends CLICKED on release whether or not
 * a long press already fired, so a CLICKED handler here would recall over the
 * value the hold had just stored — every store would look like a no-op.
 * SHORT_CLICKED is the one gated on `long_pr_sent == 0`, which is exactly the
 * distinction this pair of gestures needs, so no manual suppression flag. */
static void reg_tap_cb(lv_event_t *e) {
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

/* Returns the button: two keys (DEG/RAD and INV) show state as well as send
 * one, so the caller keeps a handle on them for refresh(). */
static lv_obj_t *make_key(calc_mode_state_t *st, lv_obj_t *pad, const char *text,
                          calc_key_t key, const lv_font_t *font, lv_color_t bg,
                          lv_color_t fg, int col, int row, int cols, int rows) {
    lv_obj_t *btn = make_button(pad, text, font, bg, fg, KEY_X(col), KEY_Y(row),
                                KEY_W * cols + KEY_GAP * (cols - 1),
                                KEY_H * rows + KEY_GAP * (rows - 1), key_cb, st);
    lv_obj_set_user_data(btn, (void *)(intptr_t)key);
    return btn;
}

/* --- screen --------------------------------------------------------------- */

static void build_readouts(calc_mode_state_t *st, lv_obj_t *scr) {
    lv_obj_t *p = make_panel(scr, READOUT_X, ZONE_Y, READOUT_W, ZONE_H);

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
    lv_obj_t *p = make_panel(scr, REG_X, ZONE_Y, REG_W, ZONE_H);

    for (int i = 0; i < CALC_REGS; i++) {
        int y = REG_ROW_Y(i);
        st->reg_refs[i].st = st;
        st->reg_refs[i].idx = i;

        /* The whole row is the button — 240x60 px, 31x7.8 mm. Two 84 px
         * buttons used to live to the right of this label; they are what paid
         * for the keypad's two extra columns. */
        lv_obj_t *row = lv_button_create(p);
        lv_obj_set_pos(row, 8, y);
        lv_obj_set_size(row, REG_W - 16, REG_ROW_H);
        lv_obj_set_style_bg_color(row, COLOR_KEY_FN, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, COLOR_PANEL_HI,
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
        lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(row, reg_tap_cb, LV_EVENT_SHORT_CLICKED,
                            &st->reg_refs[i]);
        lv_obj_add_event_cb(row, reg_hold_cb, LV_EVENT_LONG_PRESSED,
                            &st->reg_refs[i]);

        char name[4];
        snprintf(name, sizeof(name), "R%d", i);
        lv_obj_t *cap =
            make_label(row, name, &lv_font_montserrat_20, COLOR_SECONDARY);
        lv_obj_align(cap, LV_ALIGN_LEFT_MID, 12, 0);

        st->reg_labels[i] = make_label(row, "---", &lv_font_montserrat_28,
                                       COLOR_MUTED);
        lv_label_set_long_mode(st->reg_labels[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(st->reg_labels[i], REG_W - 16 - 56 - 12);
        lv_obj_set_style_text_align(st->reg_labels[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(st->reg_labels[i], LV_ALIGN_RIGHT_MID, -12, 0);
    }

    /* Tap-to-recall and hold-to-store are not discoverable the way two labelled
     * buttons were, so the panel says what they are. ASCII only: Montserrat has
     * no U+00B7, and a separator that draws as a box is the bug this repo
     * already filters out of button labels. */
    lv_obj_t *hint = make_label(p, "tap RCL / hold STO", &lv_font_montserrat_14,
                                COLOR_MUTED);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void build_keypad(calc_mode_state_t *st, lv_obj_t *scr) {
    lv_obj_t *pad = make_panel(scr, PAD_X, ZONE_Y, PAD_W, ZONE_H);
    const lv_font_t *num = &lv_font_montserrat_36;
    const lv_font_t *fn = &lv_font_montserrat_28;

    /* 9 cols x 4 rows:
     *
     *   sin  x^2   7  8  9   /   BKSP  CE   C
     *   cos  x^3   4  5  6   x   pi    e    INV
     *   tan  x^y   1  2  3   -   sqrt  [    =    ]
     *   DEG  +/-   0     .   +   1/x   [    =    ]
     */

    /* col 0: trigonometry, with the angle mode beneath it — the mode belongs
     * next to the keys it governs, not across the panel from them. */
    make_key(st, pad, "sin", CALC_KEY_SIN, fn, COLOR_KEY_FN, COLOR_INK, 0, 0, 1, 1);
    make_key(st, pad, "cos", CALC_KEY_COS, fn, COLOR_KEY_FN, COLOR_INK, 0, 1, 1, 1);
    make_key(st, pad, "tan", CALC_KEY_TAN, fn, COLOR_KEY_FN, COLOR_INK, 0, 2, 1, 1);
    lv_obj_t *drg =
        make_key(st, pad, "DEG", CALC_KEY_DRG, fn, COLOR_KEY_FN, COLOR_INK, 0, 3, 1, 1);
    st->drg_label = lv_obj_get_child(drg, 0);

    /* col 1: powers + sign */
    make_key(st, pad, "x^2", CALC_KEY_SQR, fn, COLOR_KEY_FN, COLOR_INK, 1, 0, 1, 1);
    make_key(st, pad, "x^3", CALC_KEY_CUBE, fn, COLOR_KEY_FN, COLOR_INK, 1, 1, 1, 1);
    make_key(st, pad, "x^y", CALC_KEY_POW, fn, COLOR_KEY_FN, COLOR_INK, 1, 2, 1, 1);
    make_key(st, pad, "+/-", CALC_KEY_NEG, fn, COLOR_KEY_FN, COLOR_INK, 1, 3, 1, 1);

    /* cols 2-4: the numpad island (phone layout, muscle memory intact) */
    static const struct { const char *t; calc_key_t k; int col, row; } digits[] = {
        {"7", CALC_KEY_D7, 2, 0}, {"8", CALC_KEY_D8, 3, 0}, {"9", CALC_KEY_D9, 4, 0},
        {"4", CALC_KEY_D4, 2, 1}, {"5", CALC_KEY_D5, 3, 1}, {"6", CALC_KEY_D6, 4, 1},
        {"1", CALC_KEY_D1, 2, 2}, {"2", CALC_KEY_D2, 3, 2}, {"3", CALC_KEY_D3, 4, 2},
    };
    for (unsigned i = 0; i < sizeof(digits) / sizeof(digits[0]); i++)
        make_key(st, pad, digits[i].t, digits[i].k, num, COLOR_KEY_NUM,
                 COLOR_INK, digits[i].col, digits[i].row, 1, 1);
    make_key(st, pad, "0", CALC_KEY_D0, num, COLOR_KEY_NUM, COLOR_INK, 2, 3, 2, 1);
    make_key(st, pad, ".", CALC_KEY_DOT, num, COLOR_KEY_NUM, COLOR_INK, 4, 3, 1, 1);

    /* col 5: binary ops */
    make_key(st, pad, "/", CALC_KEY_DIV, num, COLOR_KEY_OP, COLOR_INK, 5, 0, 1, 1);
    make_key(st, pad, "x", CALC_KEY_MUL, num, COLOR_KEY_OP, COLOR_INK, 5, 1, 1, 1);
    make_key(st, pad, "-", CALC_KEY_SUB, num, COLOR_KEY_OP, COLOR_INK, 5, 2, 1, 1);
    make_key(st, pad, "+", CALC_KEY_ADD, num, COLOR_KEY_OP, COLOR_INK, 5, 3, 1, 1);

    /* col 6: edit, constants, and the two new unary keys */
    make_key(st, pad, LV_SYMBOL_BACKSPACE, CALC_KEY_BACKSPACE, fn, COLOR_KEY_FN,
             COLOR_INK, 6, 0, 1, 1);
    make_key(st, pad, "pi", CALC_KEY_PI, fn, COLOR_KEY_FN, COLOR_INK, 6, 1, 1, 1);
    make_key(st, pad, "sqrt", CALC_KEY_SQRT, fn, COLOR_KEY_FN, COLOR_INK, 6, 2, 1, 1);
    make_key(st, pad, "1/x", CALC_KEY_RECIP, fn, COLOR_KEY_FN, COLOR_INK, 6, 3, 1, 1);

    /* cols 7-8: the clears, e, INV, and a big "=".
     *
     * CE is a plain function key and C keeps the danger colour: the whole
     * reason CE exists is that one of these two throws the sum away and the
     * other does not, so they must not look alike. */
    make_key(st, pad, "CE", CALC_KEY_CE, fn, COLOR_KEY_FN, COLOR_INK, 7, 0, 1, 1);
    make_key(st, pad, "C", CALC_KEY_CLEAR, fn, COLOR_KEY_DANGER, COLOR_INK,
             8, 0, 1, 1);
    make_key(st, pad, "e", CALC_KEY_E, fn, COLOR_KEY_FN, COLOR_INK, 7, 1, 1, 1);
    st->inv_btn =
        make_key(st, pad, "INV", CALC_KEY_INV, fn, COLOR_KEY_FN, COLOR_INK, 8, 1, 1, 1);
    make_key(st, pad, "=", CALC_KEY_EQ, &lv_font_montserrat_48, COLOR_EQ,
             COLOR_INK, 7, 2, 2, 2);
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
    /* Not in _create: modes are built before the state store is loaded, and the
     * file is optional anyway — so the read is attempted on the way in and
     * retried on a later activate if it did not land. */
    load_regs(st);
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
