/**
 * @file clock.c
 * Clock mode, rebuilt on the shared dual-clock widget (WI #1136).
 *
 * Three panels across the 1920x440 landscape screen:
 *   left   — the **shared** dual clock (clock_widget.h), the same widget the
 *            Launcher's side pane renders, here handed a pane big enough that
 *            clock_core's type scale picks its large tier
 *   middle — the almanac: the long date, the ISO week and the day of the year,
 *            plus any extra world-clock faces the device configured
 *   right  — the stopwatch (stopwatch.h), unchanged in behaviour
 *
 * ## Why a widget and not a second clock
 *
 * This mode used to format its own time with its own strftime calls, which is
 * how the panel came to have two clock implementations that could disagree.
 * The widget was built shared from the start for exactly this (see
 * clock_widget.h); this mode now only *places* it. Everything about how a
 * clock face looks lives in one file, and the type scale is arithmetic in the
 * pure core rather than a constant in either caller.
 *
 * ## Extra zones are configuration, not a guess
 *
 * `KDESKDASH_CLOCK_ZONES` adds world-clock rows to the almanac panel —
 * `"Asia/Tokyo,HQ=Europe/London"`. It defaults to empty, and the panel is laid
 * out to look deliberate either way. Parsing is in the pure core and every
 * malformed entry is skipped, so a typo costs one row rather than the mode.
 */
#include "modes/clock.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "clock_core.h"
#include "clock_widget.h"
#include "lvgl.h"
#include "stopwatch.h"
/* "../palette.h": src/modes/palette.h shadows the core header from in here —
 * docs/solutions/best-practices/quote-include-core-header-shadowing.md. */
#include "../palette.h"

/* Local aliases onto the named palette (src/palette.h) — the palette is the
 * single source of truth for every color here. */
#define PAL(name) lv_color_hex(kd_pal_rgb(KD_PAL_##name))
#define COLOR_BG       PAL(VOID)
#define COLOR_PANEL    PAL(DEEP_SLATE)
#define COLOR_PANEL_HI PAL(RAISED_SLATE)
#define COLOR_HAIRLINE PAL(GUNMETAL_SEAM)
#define COLOR_INK      PAL(MOON_INK)
#define COLOR_SECONDARY PAL(STEEL_MIST)
#define COLOR_CAPTION  PAL(CAPTION_HAZE)
#define COLOR_MUTED    PAL(FADED_DENIM)
#define COLOR_ZONE     PAL(UTC_FROST)
#define COLOR_SW       PAL(STOPWATCH_LIME)
#define COLOR_KEY      PAL(QUIET_KEY)

/* Timezone for the "local" clock, independent of the device's system TZ. The
 * widget pins the same zone; this mode pins it too because the almanac is
 * formatted from the process TZ and must not depend on build order. */
#define CLOCK_LOCAL_TZ "America/Los_Angeles"

/* Device-configurable extra world-clock faces. */
#define ENV_CLOCK_ZONES "KDESKDASH_CLOCK_ZONES"

/* Panel geometry. The three panels and their gutters tile 1920 exactly:
 * 12 + 620 | 12 | 640 | 12 | 612 + 12. The left pane is 620x424, comfortably
 * over clock_core's large-tier threshold (520x380). */
#define ZONE_Y 8
#define ZONE_H 424
#define CLOCK_X 12
#define CLOCK_W 620
#define ALMANAC_X 644
#define ALMANAC_W 640
#define SW_X 1296
#define SW_W 612

typedef struct {
    kd_clock_widget_t *clock;

    /* Almanac panel */
    lv_obj_t *date_label;
    lv_obj_t *almanac_label;
    lv_obj_t *zone_time[KD_CLOCK_ZONES_MAX];
    kd_clock_zone_t zones[KD_CLOCK_ZONES_MAX];
    int       zone_count;
    /* Progress rows, built only when no zones are configured (see
     * build_almanac). NULL means "this panel is showing zones instead". */
    lv_obj_t *prog_bar[3];
    lv_obj_t *prog_pct[3];

    /* Stopwatch panel */
    lv_obj_t   *sw_label;
    lv_obj_t   *start_btn_label;
    stopwatch_t sw;
    int         sw_digits; /* minute digits the readout is currently pinned to */

    /* Repaint gates: the main loop ticks at ~100 Hz and almost nothing here
     * changes that fast. The stopwatch wants tenths; the date wants seconds;
     * the world clocks show HH:MM and so want minutes — and theirs is the one
     * that matters, because each zone face costs a setenv + tzset pair and
     * that is file I/O on the UI thread. */
    time_t last_second;
    int    last_minute;
} clock_state_t;

/* Monotonic milliseconds for the stopwatch (independent of wall-clock jumps). */
static uint32_t now_ms(void) {
    return lv_tick_get();
}

/* --- the stopwatch readout, pinned so it cannot shove its neighbours ------ */

/* No font in this build has tabular digits, so a ticking readout changes width
 * as its digits change — and a label that changes width moves whatever shares
 * its row or hangs off its alignment. The fix is the documented one
 * (docs/solutions/best-practices/proportional-digits-move-their-neighbours.md):
 * pin the box to the widest rendering and let the digits shuffle inside it.
 *
 * "Widest rendering" depends on how many minute digits are showing, which is
 * why this takes the elapsed time: pinning to a fixed "M:SS.s" would clip at
 * ten minutes, and pinning to some generous maximum would leave the readout
 * visibly off-centre for the first nine. The width is therefore recomputed
 * only when the digit count changes — twice an hour, not ten times a second. */
static void pin_stopwatch_width(clock_state_t *st, uint32_t elapsed_ms) {
    int minutes = (int)(elapsed_ms / 60000u);
    int digits = 1;
    for (int m = minutes; m >= 10; m /= 10)
        digits++;
    if (digits == st->sw_digits)
        return;
    st->sw_digits = digits;

    const lv_font_t *font =
        lv_obj_get_style_text_font(st->sw_label, LV_PART_MAIN);

    /* Widest digit, found rather than assumed — it is a per-font fact, and
     * guessing wrong reintroduces the wander for exactly the digits that are
     * worst. Same reasoning as clock_widget.c's seconds field. */
    char     widest = '0';
    uint16_t best = 0;
    for (char d = '0'; d <= '9'; d++) {
        uint16_t adv = lv_font_get_glyph_width(font, (uint32_t)d, 0);
        if (adv > best) {
            best = adv;
            widest = d;
        }
    }

    /* <digits> minutes, ':', two seconds, '.', one tenth. */
    char pattern[24];
    int  n = 0;
    for (int i = 0; i < digits && n < (int)sizeof(pattern) - 5; i++)
        pattern[n++] = widest;
    pattern[n++] = ':';
    pattern[n++] = widest;
    pattern[n++] = widest;
    pattern[n++] = '.';
    pattern[n++] = widest;
    pattern[n] = '\0';

    int32_t ls = lv_obj_get_style_text_letter_space(st->sw_label, LV_PART_MAIN);
    lv_obj_set_width(st->sw_label, lv_text_get_width(pattern, (uint32_t)n, font, ls));
}

static void refresh_stopwatch_label(clock_state_t *st) {
    uint32_t elapsed = stopwatch_elapsed_ms(&st->sw, now_ms());
    pin_stopwatch_width(st, elapsed);
    char buf[16];
    stopwatch_format(elapsed, buf, sizeof(buf));
    lv_label_set_text(st->sw_label, buf);
}

/* --- event callbacks ------------------------------------------------------ */

/* A swipe that starts on a button still releases over it; ignore those so
 * shell navigation and the buttons don't fight (see the best-practice doc). */
static bool is_swipe(void) {
    lv_indev_t *indev = lv_indev_active();
    return indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE;
}

static void start_stop_cb(lv_event_t *e) {
    if (is_swipe())
        return;
    clock_state_t *st = lv_event_get_user_data(e);
    stopwatch_toggle(&st->sw, now_ms());
    lv_label_set_text(st->start_btn_label, st->sw.running ? "Stop" : "Start");
    refresh_stopwatch_label(st);
}

static void reset_cb(lv_event_t *e) {
    if (is_swipe())
        return;
    clock_state_t *st = lv_event_get_user_data(e);
    stopwatch_reset(&st->sw, now_ms());
    lv_label_set_text(st->start_btn_label, st->sw.running ? "Stop" : "Start");
    refresh_stopwatch_label(st);
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

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int x, int y,
                             lv_event_cb_t cb, void *user_data,
                             lv_obj_t **out_label) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 150, 70);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, COLOR_KEY, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, COLOR_PANEL_HI,
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *label = make_label(btn, text, &kd_font_montserrat_28, COLOR_INK);
    lv_obj_center(label);
    if (out_label)
        *out_label = label;
    return btn;
}

/* A caption above a readout: the small grey word that says what it is. */
static void make_caption(lv_obj_t *parent, const char *text, lv_align_t align,
                         int dx, int dy) {
    lv_obj_t *c = make_label(parent, text, &kd_font_montserrat_20, COLOR_CAPTION);
    lv_obj_align(c, align, dx, dy);
}

/* One "Day [========----] 62%" row. */
static void build_progress_row(clock_state_t *st, lv_obj_t *p, int slot,
                               const char *label, int y) {
    lv_obj_t *l = make_label(p, label, &kd_font_montserrat_20, COLOR_MUTED);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 20, y + 4);

    lv_obj_t *bar = lv_bar_create(p);
    lv_obj_set_size(bar, ALMANAC_W - 40 - 80 - 70, 14);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 20 + 80, y + 8);
    lv_obj_set_style_bg_color(bar, COLOR_PANEL_HI, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COLOR_ZONE, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 7, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 7, LV_PART_INDICATOR);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    st->prog_bar[slot] = bar;

    /* The percentage is the volatile part: "9%" and "99%" are different widths
     * in a proportional font, so it is right-aligned against the panel edge
     * and grows leftward into its own empty space rather than pushing the bar.
     * Same rule as the stopwatch readout, one anchor away. */
    st->prog_pct[slot] = make_label(p, "0%", &kd_font_montserrat_20, COLOR_SECONDARY);
    lv_obj_set_width(st->prog_pct[slot], 60);
    lv_obj_set_style_text_align(st->prog_pct[slot], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(st->prog_pct[slot], LV_ALIGN_TOP_RIGHT, -20, y + 4);
}

static void build_almanac(clock_state_t *st, lv_obj_t *scr) {
    lv_obj_t *p = make_panel(scr, ALMANAC_X, ZONE_Y, ALMANAC_W, ZONE_H);

    make_caption(p, "Today", LV_ALIGN_TOP_LEFT, 20, 14);

    /* The long date is the headline of this panel. It is bounded and dotted
     * rather than allowed to size itself: "Wednesday 30 September 2026" is the
     * worst case and it very nearly fills the panel at this size. */
    st->date_label = make_label(p, "", &kd_font_montserrat_36, COLOR_INK);
    lv_obj_set_width(st->date_label, ALMANAC_W - 40);
    lv_label_set_long_mode(st->date_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(st->date_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(st->date_label, LV_ALIGN_TOP_LEFT, 20, 46);

    st->almanac_label = make_label(p, "", &kd_font_montserrat_20, COLOR_SECONDARY);
    lv_obj_align(st->almanac_label, LV_ALIGN_TOP_LEFT, 20, 100);

    st->zone_count =
        kd_clock_parse_zones(getenv(ENV_CLOCK_ZONES), st->zones, KD_CLOCK_ZONES_MAX);

    /* The lower two thirds of the panel go to **configured** content when there
     * is any, and to derived content when there is not — so the panel is full
     * either way and a device that names its zones is never showing filler
     * instead of them. Both fit the same space; showing both does not. */
    if (st->zone_count > 0) {
        make_caption(p, "Elsewhere", LV_ALIGN_TOP_LEFT, 20, 148);

        /* One row per configured zone: label on the left, HH:MM on the right,
         * so the times form a column the eye can scan regardless of label
         * length. */
        for (int i = 0; i < st->zone_count; i++) {
            int y = 182 + i * 56;
            lv_obj_t *name = make_label(p, st->zones[i].label,
                                        &kd_font_montserrat_28, COLOR_MUTED);
            lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
            lv_obj_set_width(name, ALMANAC_W - 40 - 160);
            lv_obj_align(name, LV_ALIGN_TOP_LEFT, 20, y);

            st->zone_time[i] =
                make_label(p, "--:--", &kd_font_montserrat_28, COLOR_ZONE);
            lv_obj_align(st->zone_time[i], LV_ALIGN_TOP_RIGHT, -20, y);
        }
        return;
    }

    make_caption(p, "Elapsed", LV_ALIGN_TOP_LEFT, 20, 160);
    build_progress_row(st, p, 0, "Day", 200);
    build_progress_row(st, p, 1, "Week", 256);
    build_progress_row(st, p, 2, "Year", 312);
}

static void build_stopwatch(clock_state_t *st, lv_obj_t *scr) {
    lv_obj_t *p = make_panel(scr, SW_X, ZONE_Y, SW_W, ZONE_H);

    make_caption(p, "Stopwatch", LV_ALIGN_TOP_MID, 0, 40);

    st->sw_label = make_label(p, "0:00.0", &kd_font_montserrat_48, COLOR_SW);
    /* Left-aligned inside a box pinned to the widest rendering: the leading
     * digit stays put and the shuffle happens to its right, into space that is
     * already part of the box. The box itself never resizes, so the caption
     * above and the buttons below never move. */
    lv_obj_set_style_text_align(st->sw_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(st->sw_label, LV_ALIGN_TOP_MID, 0, 100);
    pin_stopwatch_width(st, 0);

    int bx = (SW_W - (150 * 2 + 20)) / 2;
    make_button(p, "Start", bx, 240, start_stop_cb, st, &st->start_btn_label);
    make_button(p, "Reset", bx + 170, 240, reset_cb, st, NULL);
}

static void build_screen(kd_mode_t *self) {
    clock_state_t *st = self->state;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* The shared widget measures the container it is handed, so the container
     * gets its final size before the widget is built inside it. */
    lv_obj_t *clock_pane = make_panel(scr, CLOCK_X, ZONE_Y, CLOCK_W, ZONE_H);
    st->clock = kd_clock_widget_create(clock_pane);

    build_almanac(st, scr);
    build_stopwatch(st, scr);

    self->screen = scr;
}

static void activate(kd_mode_t *self) {
    clock_state_t *st = self->state;
    if (!self->screen)
        build_screen(self);
    /* The stopwatch kept running while hidden; resync the readout and label.
     * The date and the world clocks are stale for at most one tick, so they
     * are left to the ordinary gates rather than forced here. */
    lv_label_set_text(st->start_btn_label, st->sw.running ? "Stop" : "Start");
    refresh_stopwatch_label(st);
    st->last_second = 0;
    st->last_minute = -1;
}

static void tick(kd_mode_t *self) {
    clock_state_t *st = self->state;
    if (!self->screen)
        return;

    /* Every tick: the shared widget (which caches its own labels internally
     * and only touches the ones whose text changed) and the stopwatch, which
     * is the only readout here that genuinely moves at tenths. */
    kd_clock_widget_tick(st->clock);
    refresh_stopwatch_label(st);

    time_t t = time(NULL);
    if (t == st->last_second)
        return;
    st->last_second = t;

    kd_clock_almanac_t a;
    kd_clock_almanac(t, &a);
    lv_label_set_text(st->date_label, a.long_date);
    lv_label_set_text_fmt(st->almanac_label, "Week %d   Day %d of %d",
                          a.iso_week, a.yday, a.ydays);

    if (st->zone_count == 0) {
        const int pct[3] = {a.day_pct, a.week_pct, a.year_pct};
        for (int i = 0; i < 3; i++) {
            lv_bar_set_value(st->prog_bar[i], pct[i], LV_ANIM_OFF);
            lv_label_set_text_fmt(st->prog_pct[i], "%d%%", pct[i]);
        }
        return;
    }

    /* Minute-gated: these faces show HH:MM, and each one costs a setenv and
     * two tzset calls — file I/O we are not spending once a second for a
     * readout that changes once a minute. */
    struct tm tm_local;
    localtime_r(&t, &tm_local);
    if (tm_local.tm_min == st->last_minute)
        return;
    st->last_minute = tm_local.tm_min;

    for (int i = 0; i < st->zone_count; i++) {
        kd_clock_face_t f;
        kd_clock_zone_face(t, st->zones[i].tz, &f);
        lv_label_set_text(st->zone_time[i], f.hm);
    }
}

kd_mode_t *clock_mode_create(const char *id, const char *title) {
    /* Pin the local clock's timezone regardless of the device's system TZ.
     * kd_clock_widget_create does this too; doing it here as well keeps the
     * almanac correct without depending on which is built first. */
    setenv("TZ", CLOCK_LOCAL_TZ, 1);
    tzset();

    kd_mode_t *m = calloc(1, sizeof(*m));
    clock_state_t *st = calloc(1, sizeof(*st));
    stopwatch_init(&st->sw);
    st->sw_digits = -1; /* nothing pinned yet */
    st->last_minute = -1;
    m->id = id;
    m->title = title;
    m->state = st;
    m->activate = activate;
    m->deactivate = NULL; /* nothing runs while hidden: tick is not called */
    m->tick = tick;
    return m;
}
