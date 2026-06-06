/**
 * @file clock.c
 * Clock mode: large centered local time, UTC on the left, and a wall-clock
 * stopwatch on the right. The stopwatch core (stopwatch.c) is wall-clock based,
 * so elapsed time stays correct while the mode is hidden; only the on-screen
 * readout pauses updating while inactive.
 */
#include "modes/clock.h"

#include <stdlib.h>
#include <time.h>

#include "lvgl.h"
#include "stopwatch.h"

/* Timezone for the "local" clock, independent of the device's system TZ. */
#define CLOCK_LOCAL_TZ "America/Los_Angeles"

/* Panel calibration (measured with a mm ruler against the 1920x440 screen):
 * the 150 px stopwatch buttons span 19.5 mm, i.e. ~7.69 px/mm. Use MM_TO_PX
 * to place things in real-world millimetres. */
#define PX_PER_MM 7.69f
#define MM_TO_PX(mm) ((int)((mm) * PX_PER_MM))

typedef struct {
    lv_obj_t   *local_label; /* HH:MM, large */
    lv_obj_t   *local_sec_label; /* :SS, smaller, second line */
    lv_obj_t   *utc_label;
    lv_obj_t   *sw_label;
    lv_obj_t   *start_btn_label;
    stopwatch_t sw;
} clock_state_t;

/* Monotonic milliseconds for the stopwatch (independent of wall-clock jumps). */
static uint32_t now_ms(void) {
    return lv_tick_get();
}

static void refresh_stopwatch_label(clock_state_t *st) {
    char buf[16];
    stopwatch_format(stopwatch_elapsed_ms(&st->sw, now_ms()), buf, sizeof(buf));
    lv_label_set_text(st->sw_label, buf);
}

static void start_stop_cb(lv_event_t *e) {
    clock_state_t *st = lv_event_get_user_data(e);
    stopwatch_toggle(&st->sw, now_ms());
    lv_label_set_text(st->start_btn_label, st->sw.running ? "Stop" : "Start");
    refresh_stopwatch_label(st);
}

static void reset_cb(lv_event_t *e) {
    clock_state_t *st = lv_event_get_user_data(e);
    stopwatch_reset(&st->sw, now_ms());
    lv_label_set_text(st->start_btn_label, st->sw.running ? "Stop" : "Start");
    refresh_stopwatch_label(st);
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int x, int y,
                             lv_event_cb_t cb, void *user_data,
                             lv_obj_t **out_label) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 150, 70);
    lv_obj_set_pos(btn, x, y);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_center(label);
    if (out_label)
        *out_label = label;
    return btn;
}

static void build_screen(kd_mode_t *self) {
    clock_state_t *st = self->state;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x05070d), LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Centered large local time on two lines: HH:MM big, :SS smaller below.
     * Montserrat tops out at 48 px, so scale the HH:MM label up (256 = 1.0x)
     * around its centre to make it "quite a bit bigger". */
    st->local_label = lv_label_create(scr);
    lv_obj_set_style_text_font(st->local_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->local_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_transform_scale(st->local_label, 512, LV_PART_MAIN); /* 2.0x */
    lv_obj_set_style_transform_pivot_x(st->local_label, lv_pct(50), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(st->local_label, lv_pct(50), LV_PART_MAIN);
    lv_label_set_text(st->local_label, "--:--");
    lv_obj_align(st->local_label, LV_ALIGN_CENTER, 0, -70);

    st->local_sec_label = lv_label_create(scr);
    lv_obj_set_style_text_font(st->local_sec_label, &lv_font_montserrat_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->local_sec_label, lv_color_hex(0xb9c6db), LV_PART_MAIN);
    lv_label_set_text(st->local_sec_label, ":--");
    lv_obj_align(st->local_sec_label, LV_ALIGN_CENTER, 0, 60);

    /* UTC time on the left, no seconds (seconds match local across zones). */
    lv_obj_t *utc_caption = lv_label_create(scr);
    lv_label_set_text(utc_caption, "UTC");
    lv_obj_set_style_text_font(utc_caption, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(utc_caption, lv_color_hex(0x7c93b3), LV_PART_MAIN);
    lv_obj_align(utc_caption, LV_ALIGN_LEFT_MID, 60, -50);

    st->utc_label = lv_label_create(scr);
    lv_obj_set_style_text_font(st->utc_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->utc_label, lv_color_hex(0xcfe0f5), LV_PART_MAIN);
    lv_label_set_text(st->utc_label, "--:--");
    lv_obj_align(st->utc_label, LV_ALIGN_LEFT_MID, 60, 10);

    /* Stopwatch on the right: readout + Start/Stop and Reset buttons. */
    lv_obj_t *sw_caption = lv_label_create(scr);
    lv_label_set_text(sw_caption, "Stopwatch");
    lv_obj_set_style_text_font(sw_caption, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(sw_caption, lv_color_hex(0x7c93b3), LV_PART_MAIN);
    lv_obj_align(sw_caption, LV_ALIGN_RIGHT_MID, -60, -110);

    st->sw_label = lv_label_create(scr);
    lv_obj_set_style_text_font(st->sw_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->sw_label, lv_color_hex(0x6ddf6d), LV_PART_MAIN);
    lv_label_set_text(st->sw_label, "0:00.0");
    lv_obj_align(st->sw_label, LV_ALIGN_RIGHT_MID, -60, -50);

    /* Buttons sit ~15 mm to the right of where they started, under the
     * stopwatch readout (positions measured in mm via MM_TO_PX). */
    make_button(scr, "Start", 1430 + MM_TO_PX(15), 250, start_stop_cb, st,
                &st->start_btn_label);
    make_button(scr, "Reset", 1600 + MM_TO_PX(15), 250, reset_cb, st, NULL);

    self->screen = scr;
}

static void activate(kd_mode_t *self) {
    clock_state_t *st = self->state;
    if (!self->screen)
        build_screen(self);
    /* The stopwatch kept running while hidden; resync the readout and label. */
    lv_label_set_text(st->start_btn_label, st->sw.running ? "Stop" : "Start");
    refresh_stopwatch_label(st);
}

static void tick(kd_mode_t *self) {
    clock_state_t *st = self->state;
    if (!self->screen)
        return;

    time_t t = time(NULL);
    struct tm tm_local, tm_utc;
    char buf[32];

    localtime_r(&t, &tm_local);
    strftime(buf, sizeof(buf), "%H:%M", &tm_local);
    lv_label_set_text(st->local_label, buf);
    strftime(buf, sizeof(buf), ":%S", &tm_local);
    lv_label_set_text(st->local_sec_label, buf);

    gmtime_r(&t, &tm_utc);
    strftime(buf, sizeof(buf), "%H:%M", &tm_utc);
    lv_label_set_text(st->utc_label, buf);

    refresh_stopwatch_label(st);
}

kd_mode_t *clock_mode_create(const char *id, const char *title) {
    /* Pin the local clock's timezone regardless of the device's system TZ. */
    setenv("TZ", CLOCK_LOCAL_TZ, 1);
    tzset();

    kd_mode_t *m = calloc(1, sizeof(*m));
    clock_state_t *st = calloc(1, sizeof(*st));
    stopwatch_init(&st->sw);
    m->id = id;
    m->title = title;
    m->state = st;
    m->activate = activate;
    m->deactivate = NULL;
    m->tick = tick;
    return m;
}
