/**
 * @file clock_widget.c
 * Shared dual-clock widget — see clock_widget.h. LVGL glue only; the
 * formatting and the type-scale choice are in the pure clock_core.
 */
#include "clock_widget.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "clock_core.h"
#include "palette.h"

/* Timezone for the "local" face, independent of the device's system TZ. */
#define CLOCK_LOCAL_TZ "America/Los_Angeles"

#define PAL(name) lv_color_hex(kd_pal_rgb(KD_PAL_##name))

/* One tier's type scale. Montserrat is a bitmap font at fixed sizes, so a tier
 * is a choice among the five built in — not an arbitrary point size. */
typedef struct {
    const lv_font_t *hm;      /* the big local HH:MM      */
    const lv_font_t *sec;     /* its :SS                  */
    const lv_font_t *utc_hm;  /* the second face          */
    const lv_font_t *caption; /* date lines, zone labels  */
    bool             show_dates;
} tier_fonts_t;

static tier_fonts_t tier_fonts(kd_clock_tier_t t) {
    switch (t) {
    case KD_CLOCK_TIER_L:
        return (tier_fonts_t){&lv_font_montserrat_48, &lv_font_montserrat_28,
                              &lv_font_montserrat_36, &lv_font_montserrat_20, true};
    case KD_CLOCK_TIER_M:
        return (tier_fonts_t){&lv_font_montserrat_48, &lv_font_montserrat_20,
                              &lv_font_montserrat_28, &lv_font_montserrat_14, true};
    case KD_CLOCK_TIER_S:
        return (tier_fonts_t){&lv_font_montserrat_36, &lv_font_montserrat_14,
                              &lv_font_montserrat_20, &lv_font_montserrat_14, false};
    default:
        return (tier_fonts_t){&lv_font_montserrat_28, &lv_font_montserrat_14,
                              &lv_font_montserrat_20, &lv_font_montserrat_14, false};
    }
}

struct kd_clock_widget {
    lv_obj_t *local_hm, *local_sec, *local_date;
    lv_obj_t *utc_hm, *utc_date;
    bool      show_dates;

    /* Last text pushed to each label, so a 100 Hz tick does not invalidate
     * five labels a hundred times a second to redraw the same digits. These
     * hold the *composed* strings, not the raw face fields — a cache too small
     * to hold what was written would mismatch forever and defeat itself. */
    char c_local_hm[8], c_local_sec[8], c_local_date[48];
    char c_utc_hm[8], c_utc_date[24];
    bool primed;
};

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                            lv_color_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

/* A transparent, non-scrolling, gesture-transparent flex box — the widget lives
 * inside the swipe-navigated shell, so nothing it builds may swallow a swipe. */
static lv_obj_t *make_box(lv_obj_t *parent, lv_flex_flow_t flow,
                          lv_flex_align_t cross) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, flow);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, cross, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box, 6, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return box;
}

kd_clock_widget_t *kd_clock_widget_create(lv_obj_t *parent) {
    if (!parent)
        return NULL;
    kd_clock_widget_t *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;

    setenv("TZ", CLOCK_LOCAL_TZ, 1);
    tzset();

    /* Measure the space we were actually given rather than assuming a panel. */
    lv_obj_update_layout(parent);
    tier_fonts_t f = tier_fonts(
        kd_clock_tier(lv_obj_get_width(parent), lv_obj_get_height(parent)));
    w->show_dates = f.show_dates;

    /* Spread the blocks down the pane rather than stacking them tightly in the
     * middle: Montserrat tops out at 48 px, so the way to use a tall pane is
     * spacing, not type size. (Making a full-screen clock genuinely *fill* its
     * screen is WI #1136's job — it owns the clock-mode rebuild.) */
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(parent, 2, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* Two groups, not four loose lines: each face keeps its own date tucked
     * under it, and the spacing goes *between* the faces. Spread across the
     * pane, ungrouped rows read as unrelated readouts. */

    /* Local face: HH:MM with :SS riding its baseline, date beneath. */
    lv_obj_t *local_box = make_box(parent, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *row = make_box(local_box, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_END);
    w->local_hm = make_label(row, f.hm, PAL(MOON_INK), "--:--");
    w->local_sec = make_label(row, f.sec, PAL(STEEL_MIST), ":--");
    if (f.show_dates)
        w->local_date = make_label(local_box, f.caption, PAL(STEEL_MIST), "");

    /* UTC face, visibly the secondary one: smaller, cooler, its own caption.
     * On a winter evening the two faces show different *days*, which is the
     * reason both are here at all. */
    lv_obj_t *utc_box = make_box(parent, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *urow = make_box(utc_box, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_END);
    make_label(urow, f.caption, PAL(FADED_DENIM), "UTC");
    w->utc_hm = make_label(urow, f.utc_hm, PAL(UTC_FROST), "--:--");
    if (f.show_dates)
        w->utc_date = make_label(utc_box, f.caption, PAL(FADED_DENIM), "");

    return w;
}

/* Set a label only when its text changed — see the `last_*` note above. */
static void set_if_changed(lv_obj_t *label, char *cache, size_t cachesz,
                           const char *text, bool force) {
    if (!label)
        return;
    if (!force && strcmp(cache, text) == 0)
        return;
    snprintf(cache, cachesz, "%s", text);
    lv_label_set_text(label, text);
}

void kd_clock_widget_tick(kd_clock_widget_t *w) {
    if (!w)
        return;
    time_t t = time(NULL);
    kd_clock_face_t local, utc;
    kd_clock_local_face(t, &local);
    kd_clock_utc_face(t, &utc);

    bool force = !w->primed;
    w->primed = true;

    set_if_changed(w->local_hm, w->c_local_hm, sizeof(w->c_local_hm), local.hm,
                   force);
    set_if_changed(w->local_sec, w->c_local_sec, sizeof(w->c_local_sec),
                   local.sec, force);
    set_if_changed(w->utc_hm, w->c_utc_hm, sizeof(w->c_utc_hm), utc.hm, force);

    if (w->show_dates) {
        /* The date line carries the zone too: "Sat 18 Jul  PDT". ASCII only —
         * the built-in Montserrat has no U+00B7, and a separator that draws as
         * a box is the same bug this mode filters out of button labels. */
        char line[sizeof(w->c_local_date)];
        snprintf(line, sizeof(line), "%s  %s", local.date, local.zone);
        set_if_changed(w->local_date, w->c_local_date, sizeof(w->c_local_date),
                       line, force);

        /* The UTC date earns its line only when it differs from the local one.
         * For most of the day it is the same string twice — noise — but on a
         * winter evening the two faces really are a day apart, and that is
         * exactly when you want to see it. */
        set_if_changed(w->utc_date, w->c_utc_date, sizeof(w->c_utc_date),
                       utc.date, force);
        if (strcmp(utc.date, local.date) == 0)
            lv_obj_add_flag(w->utc_date, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(w->utc_date, LV_OBJ_FLAG_HIDDEN);
    }
}

void kd_clock_widget_destroy(kd_clock_widget_t *w) {
    free(w);
}
