/**
 * @file claude.c
 * Claude mode: at-a-glance fleet agent activity + subscription usage limits.
 *
 * Three zones on the 1920x440 panel (design: docs/brainstorms/2026-07-02 +
 * approved mockup): AGENTS — attention-first session rows (awaiting input on
 * top, then working by recency, then idle/stale); RECENT — last completed
 * sessions; USAGE — two 270° arc gauges for the 5-hour and 7-day limits with
 * an "as of" freshness line (limits only update while some session runs).
 *
 * All ordering/derivation lives in the pure claude_feed core; this file owns
 * LVGL rendering and feed I/O cadence. Data is polled only while active.
 */
#include "modes/claude.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "claude_feed.h"
#include "claude_redis.h"
#include "lvgl.h"

#define CLAUDE_POLL_MS     2000 /* sessions + limits + recent refresh cadence */
#define CLAUDE_DISCOVER_MS 5000 /* SCAN discovery cadence */

#define CLAUDE_ROWS    5 /* session rows on screen; overflow -> "+N more" */
#define CLAUDE_RECENTS 4

/* Zone widths (px); usage takes the remainder of 1920. Hardware-calibrated
 * 2026-07-03: at 1120 the usage zone (430 - 60 padding = 370 content) clipped
 * the arc edges and the worst-case reset line ("resets Thu 05:00" needs a
 * ~176px column; 170 + 176 + 24 gap > 370). 40px ≈ 5.2mm at the panel's
 * 7.69 px/mm (see clock.c MM_TO_PX) moved to USAGE fixes both with slack. */
#define ZONE_AGENTS_W 1080
#define ZONE_RECENT_W 370
#define ROW_H         58

/* Design tokens (validated against this surface — see the plan). */
#define COLOR_BG        lv_color_hex(0x05070d)
#define COLOR_PANEL     lv_color_hex(0x0a0f1a)
#define COLOR_HAIRLINE  lv_color_hex(0x1b2334)
#define COLOR_INK       lv_color_hex(0xe9edf6)
#define COLOR_SECONDARY lv_color_hex(0x8b95ab)
#define COLOR_MUTED     lv_color_hex(0x525d73)
#define COLOR_ACCENT    lv_color_hex(0xcf6b4a) /* claude coral */
#define COLOR_WORKING   lv_color_hex(0x35a271)
#define COLOR_AWAITING  lv_color_hex(0xb9832c) /* doubles as the warn tone */

typedef struct {
    lv_obj_t *row;
    lv_obj_t *stripe;
    lv_obj_t *host;
    lv_obj_t *proj;
    lv_obj_t *model;
    lv_obj_t *status;
    lv_obj_t *age;
} claude_row_t;

typedef struct {
    lv_obj_t *box;
    lv_obj_t *title;
    lv_obj_t *meta;
} claude_recent_t;

typedef struct {
    lv_obj_t *arc;
    lv_obj_t *pct;
    lv_obj_t *reset;
} claude_gauge_t;

typedef struct {
    lv_obj_t *head_sub; /* "2 working • 1 waiting on you" */
    claude_row_t rows[CLAUDE_ROWS];
    lv_obj_t *more;     /* "+N more" under the rows */
    lv_obj_t *empty;    /* "no agents active" placeholder */

    claude_recent_t recents[CLAUDE_RECENTS];
    lv_obj_t *recent_empty;

    claude_gauge_t five;
    claude_gauge_t seven;
    lv_obj_t *asof;

    lv_obj_t *unavail; /* full-panel quiet banner when the feed is down */

    uint32_t last_poll;
    uint32_t last_discover;
} claude_state_t;

/* ---------- widget builders ---------- */

static lv_obj_t *make_zone(lv_obj_t *parent, int width, bool hairline_left) {
    lv_obj_t *z = lv_obj_create(parent);
    lv_obj_remove_style_all(z);
    if (width > 0)
        lv_obj_set_size(z, width, LV_PCT(100));
    else {
        lv_obj_set_height(z, LV_PCT(100));
        lv_obj_set_flex_grow(z, 1);
    }
    if (hairline_left) {
        lv_obj_set_style_border_color(z, COLOR_HAIRLINE, 0);
        lv_obj_set_style_border_width(z, 1, 0);
        lv_obj_set_style_border_side(z, LV_BORDER_SIDE_LEFT, 0);
    }
    lv_obj_set_style_pad_top(z, 26, 0);
    lv_obj_set_style_pad_bottom(z, 22, 0);
    lv_obj_set_style_pad_left(z, 30, 0);
    lv_obj_set_style_pad_right(z, 30, 0);
    lv_obj_set_style_pad_row(z, 10, 0);
    lv_obj_clear_flag(z, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(z, LV_FLEX_FLOW_COLUMN);
    return z;
}

/* Zone header label: coral, uppercase, letterspaced. */
static lv_obj_t *make_zone_label(lv_obj_t *parent, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, COLOR_ACCENT, 0);
    lv_obj_set_style_text_letter_space(l, 4, 0);
    return l;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                            lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

static void make_row(claude_state_t *st, lv_obj_t *parent, int i) {
    claude_row_t *r = &st->rows[i];

    r->row = lv_obj_create(parent);
    lv_obj_remove_style_all(r->row);
    lv_obj_set_size(r->row, LV_PCT(100), ROW_H);
    lv_obj_set_style_bg_color(r->row, COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(r->row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r->row, 10, 0);
    lv_obj_set_style_pad_right(r->row, 24, 0);
    lv_obj_set_style_pad_column(r->row, 22, 0);
    lv_obj_clear_flag(r->row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(r->row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r->row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    r->stripe = lv_obj_create(r->row);
    lv_obj_remove_style_all(r->stripe);
    lv_obj_set_size(r->stripe, 6, LV_PCT(100));
    lv_obj_set_style_bg_opa(r->stripe, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r->stripe, 3, 0);

    r->host = make_label(r->row, &lv_font_montserrat_20, COLOR_SECONDARY);
    lv_obj_set_width(r->host, 120);
    lv_label_set_long_mode(r->host, LV_LABEL_LONG_DOT);

    r->proj = make_label(r->row, &lv_font_montserrat_28, COLOR_INK);
    lv_obj_set_flex_grow(r->proj, 1);
    lv_label_set_long_mode(r->proj, LV_LABEL_LONG_DOT);

    r->model = make_label(r->row, &lv_font_montserrat_20, COLOR_SECONDARY);
    lv_obj_set_width(r->model, 130);
    lv_label_set_long_mode(r->model, LV_LABEL_LONG_DOT);

    r->status = make_label(r->row, &lv_font_montserrat_20, COLOR_MUTED);
    lv_obj_set_width(r->status, 290);
    lv_obj_set_style_text_letter_space(r->status, 2, 0);
    lv_label_set_long_mode(r->status, LV_LABEL_LONG_CLIP);

    r->age = make_label(r->row, &lv_font_montserrat_20, COLOR_SECONDARY);
    lv_obj_set_width(r->age, 90);
    lv_obj_set_style_text_align(r->age, LV_TEXT_ALIGN_RIGHT, 0);
}

static void make_recent(claude_state_t *st, lv_obj_t *parent, int i) {
    claude_recent_t *rc = &st->recents[i];

    rc->box = lv_obj_create(parent);
    lv_obj_remove_style_all(rc->box);
    lv_obj_set_size(rc->box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(rc->box, COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(rc->box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(rc->box, 10, 0);
    lv_obj_set_style_pad_hor(rc->box, 18, 0);
    lv_obj_set_style_pad_ver(rc->box, 10, 0);
    lv_obj_set_style_pad_row(rc->box, 3, 0);
    lv_obj_clear_flag(rc->box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(rc->box, LV_FLEX_FLOW_COLUMN);

    rc->title = make_label(rc->box, &lv_font_montserrat_20, COLOR_INK);
    lv_obj_set_width(rc->title, LV_PCT(100));
    lv_label_set_long_mode(rc->title, LV_LABEL_LONG_DOT);

    rc->meta = make_label(rc->box, &lv_font_montserrat_20, COLOR_MUTED);
    lv_obj_set_width(rc->meta, LV_PCT(100));
    lv_label_set_long_mode(rc->meta, LV_LABEL_LONG_DOT);
}

static void make_gauge(claude_gauge_t *g, lv_obj_t *parent, const char *window) {
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* 270° gauge, lv_arc's native default orientation (135° start). */
    g->arc = lv_arc_create(col);
    lv_obj_set_size(g->arc, 170, 170);
    lv_arc_set_rotation(g->arc, 135);
    lv_arc_set_bg_angles(g->arc, 0, 270);
    lv_arc_set_range(g->arc, 0, 100);
    lv_arc_set_value(g->arc, 0);
    lv_obj_remove_style(g->arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(g->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(g->arc, COLOR_HAIRLINE, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g->arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_color(g->arc, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g->arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g->arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(g->arc, true, LV_PART_INDICATOR);

    g->pct = make_label(g->arc, &lv_font_montserrat_36, COLOR_INK);
    lv_obj_center(g->pct);

    lv_obj_t *win = make_label(col, &lv_font_montserrat_20, COLOR_SECONDARY);
    lv_label_set_text(win, window);
    lv_obj_set_style_text_letter_space(win, 4, 0);

    g->reset = make_label(col, &lv_font_montserrat_20, COLOR_MUTED);
}

static void build_screen(kd_mode_t *self) {
    claude_state_t *st = self->state;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW);

    /* --- AGENTS --- */
    lv_obj_t *za = make_zone(scr, ZONE_AGENTS_W, false);

    lv_obj_t *head = lv_obj_create(za);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER);
    make_zone_label(head, "AGENTS");
    st->head_sub = make_label(head, &lv_font_montserrat_20, COLOR_SECONDARY);

    for (int i = 0; i < CLAUDE_ROWS; i++)
        make_row(st, za, i);

    st->more = make_label(za, &lv_font_montserrat_20, COLOR_MUTED);

    st->empty = make_label(za, &lv_font_montserrat_28, COLOR_MUTED);
    lv_label_set_text(st->empty, "no agents active");
    lv_obj_set_flex_grow(st->empty, 1);
    lv_obj_set_style_text_align(st->empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(st->empty, LV_PCT(100));

    /* --- RECENT --- */
    lv_obj_t *zr = make_zone(scr, ZONE_RECENT_W, true);
    lv_obj_set_style_pad_row(zr, 8, 0);
    make_zone_label(zr, "RECENT");
    for (int i = 0; i < CLAUDE_RECENTS; i++)
        make_recent(st, zr, i);
    st->recent_empty = make_label(zr, &lv_font_montserrat_20, COLOR_MUTED);
    lv_label_set_text(st->recent_empty, "nothing yet");

    /* --- USAGE --- */
    lv_obj_t *zu = make_zone(scr, 0, true);
    make_zone_label(zu, "USAGE");

    lv_obj_t *gauges = lv_obj_create(zu);
    lv_obj_remove_style_all(gauges);
    lv_obj_set_size(gauges, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(gauges, 24, 0);
    lv_obj_clear_flag(gauges, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(gauges, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(gauges, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    make_gauge(&st->five, gauges, "5 HR");
    make_gauge(&st->seven, gauges, "7 DAY");

    st->asof = make_label(zu, &lv_font_montserrat_20, COLOR_MUTED);
    lv_obj_set_width(st->asof, LV_PCT(100));
    lv_obj_set_style_text_align(st->asof, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_flex_grow(st->asof, 1);

    /* --- unreachable banner (hidden by default) --- */
    st->unavail = lv_label_create(scr);
    lv_label_set_text(st->unavail, "agent feed unavailable");
    lv_obj_set_style_text_font(st->unavail, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(st->unavail, COLOR_SECONDARY, 0);
    lv_obj_set_style_bg_color(st->unavail, COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(st->unavail, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(st->unavail, 10, 0);
    lv_obj_set_style_pad_hor(st->unavail, 40, 0);
    lv_obj_set_style_pad_ver(st->unavail, 20, 0);
    lv_obj_add_flag(st->unavail, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_center(st->unavail);

    self->screen = scr;
}

/* ---------- rendering ---------- */

static lv_color_t disp_color(cf_disp_t d) {
    switch (d) {
    case CF_DISP_AWAITING: return COLOR_AWAITING;
    case CF_DISP_WORKING:  return COLOR_WORKING;
    default:               return COLOR_MUTED;
    }
}

static void render_sessions(claude_state_t *st, cf_session_t *s, int n,
                            long long now) {
    int working = 0, waiting = 0;
    for (int i = 0; i < n; i++) {
        if (s[i].disp == CF_DISP_WORKING)
            working++;
        else if (s[i].disp == CF_DISP_AWAITING)
            waiting++;
    }

    char sub[96];
    if (n == 0)
        sub[0] = '\0';
    else if (waiting > 0)
        snprintf(sub, sizeof(sub), "%d working \xE2\x80\xA2 %d waiting on you",
                 working, waiting);
    else
        snprintf(sub, sizeof(sub), "%d working", working);
    lv_label_set_text(st->head_sub, sub);

    int shown = (n < CLAUDE_ROWS) ? n : CLAUDE_ROWS;
    for (int i = 0; i < CLAUDE_ROWS; i++) {
        claude_row_t *r = &st->rows[i];
        if (i >= shown) {
            lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(r->row, LV_OBJ_FLAG_HIDDEN);

        cf_session_t *e = &s[i];
        bool dim = (e->disp == CF_DISP_IDLE || e->disp == CF_DISP_STALE);

        lv_obj_set_style_bg_color(r->stripe, disp_color(e->disp), 0);
        lv_label_set_text(r->host, e->host);
        lv_label_set_text(r->proj, e->project);
        lv_obj_set_style_text_color(r->proj, dim ? COLOR_SECONDARY : COLOR_INK, 0);
        lv_label_set_text(r->model, e->model);
        lv_obj_set_style_text_color(r->model, dim ? COLOR_MUTED : COLOR_SECONDARY, 0);
        lv_label_set_text(r->status, cf_disp_label(e->disp));
        lv_obj_set_style_text_color(r->status, disp_color(e->disp), 0);
        lv_obj_set_style_opa(r->row,
                             e->disp == CF_DISP_STALE ? LV_OPA_70 : LV_OPA_COVER, 0);

        char age[16];
        cf_fmt_age(now - e->ts, age, sizeof(age));
        lv_label_set_text(r->age, age);
    }

    if (n > CLAUDE_ROWS) {
        char more[24];
        snprintf(more, sizeof(more), "+%d more", n - CLAUDE_ROWS);
        lv_label_set_text(st->more, more);
        lv_obj_clear_flag(st->more, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(st->more, LV_OBJ_FLAG_HIDDEN);
    }

    if (n == 0)
        lv_obj_clear_flag(st->empty, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(st->empty, LV_OBJ_FLAG_HIDDEN);
}

static void render_recent(claude_state_t *st, cf_recent_t *rec, int n,
                          long long now) {
    for (int i = 0; i < CLAUDE_RECENTS; i++) {
        claude_recent_t *rc = &st->recents[i];
        if (i >= n) {
            lv_obj_add_flag(rc->box, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(rc->box, LV_OBJ_FLAG_HIDDEN);

        /* Prefer the project name (short, recognisable); the enriched title is
         * often long — it belongs to the tooltip-less future, not 370px. */
        lv_label_set_text(rc->title, rec[i].project);

        char when[16], dur[16], meta[128];
        cf_fmt_age(now - rec[i].ended_ts, when, sizeof(when));
        if (rec[i].dur_s > 0) {
            cf_fmt_age(rec[i].dur_s, dur, sizeof(dur));
            snprintf(meta, sizeof(meta), "%s \xE2\x80\xA2 %s ago \xE2\x80\xA2 %s",
                     rec[i].host, when, dur);
        } else {
            snprintf(meta, sizeof(meta), "%s \xE2\x80\xA2 %s ago", rec[i].host, when);
        }
        lv_label_set_text(rc->meta, meta);
    }
    if (n == 0)
        lv_obj_clear_flag(st->recent_empty, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(st->recent_empty, LV_OBJ_FLAG_HIDDEN);
}

static void render_gauge(claude_gauge_t *g, float pct, long long resets_at,
                         long long now, bool valid, bool stale) {
    if (!valid) {
        lv_arc_set_value(g->arc, 0);
        lv_label_set_text(g->pct, "--");
        lv_obj_set_style_text_color(g->pct, COLOR_MUTED, 0);
        lv_label_set_text(g->reset, "");
        return;
    }
    bool warn = pct >= CF_LIMITS_WARN_PCT;
    lv_color_t c = stale ? COLOR_MUTED : (warn ? COLOR_AWAITING : COLOR_ACCENT);
    lv_arc_set_value(g->arc, (int)(pct + 0.5f));
    lv_obj_set_style_arc_color(g->arc, c, LV_PART_INDICATOR);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", (int)(pct + 0.5f));
    lv_label_set_text(g->pct, buf);
    lv_obj_set_style_text_color(
        g->pct, stale ? COLOR_MUTED : (warn ? COLOR_AWAITING : COLOR_INK), 0);

    char reset[24], line[40];
    cf_fmt_reset(resets_at, now, reset, sizeof(reset));
    snprintf(line, sizeof(line), "resets %s", reset);
    lv_label_set_text(g->reset, line);
}

static void render_limits(claude_state_t *st, const cf_limits_t *l,
                          long long now) {
    bool stale = cf_limits_stale(l, now);
    render_gauge(&st->five, l->five_pct, l->five_reset, now, l->valid, stale);
    render_gauge(&st->seven, l->seven_pct, l->seven_reset, now, l->valid, stale);

    if (!l->valid) {
        lv_label_set_text(st->asof, "no data yet");
        lv_obj_set_style_text_color(st->asof, COLOR_MUTED, 0);
    } else {
        char age[16], buf[40];
        cf_fmt_age(now - l->updated_at, age, sizeof(age));
        if (stale) {
            snprintf(buf, sizeof(buf), "stale - as of %s ago", age);
            lv_obj_set_style_text_color(st->asof, COLOR_AWAITING, 0);
        } else {
            snprintf(buf, sizeof(buf), "as of %s ago", age);
            lv_obj_set_style_text_color(st->asof, COLOR_MUTED, 0);
        }
        lv_label_set_text(st->asof, buf);
    }
}

/* ---------- mode plumbing ---------- */

static void poll(claude_state_t *st) {
    long long now = (long long)time(NULL);

    claude_key_t keys[CF_SESSIONS_MAX];
    int nkeys = claude_redis_keys(keys, CF_SESSIONS_MAX);

    cf_session_t sessions[CF_SESSIONS_MAX];
    int n = 0;
    for (int i = 0; i < nkeys; i++) {
        if (claude_redis_get_session(&keys[i], &sessions[n]) == CLAUDE_OK)
            n++; /* ABSENT (just ended / malformed) rows simply drop out */
    }
    cf_sessions_refresh(sessions, n, now);

    cf_limits_t limits;
    claude_redis_get_limits(&limits);

    cf_recent_t recent[CLAUDE_RECENTS];
    int nrec = claude_redis_get_recent(recent, CLAUDE_RECENTS);

    bool up = claude_redis_reachable();
    if (up) {
        render_sessions(st, sessions, n, now);
        render_recent(st, recent, nrec, now);
        render_limits(st, &limits, now);
        lv_obj_add_flag(st->unavail, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* Keep the last-rendered panel visible underneath; one quiet banner
         * says why nothing is moving. */
        lv_obj_clear_flag(st->unavail, LV_OBJ_FLAG_HIDDEN);
    }
}

static void activate(kd_mode_t *self) {
    if (!self->screen)
        build_screen(self);
    claude_state_t *st = self->state;
    /* Force discovery + a poll on the next tick. */
    st->last_poll = 0;
    st->last_discover = 0;
}

static void tick(kd_mode_t *self) {
    claude_state_t *st = self->state;
    if (!self->screen)
        return;

    if (st->last_discover == 0 ||
        lv_tick_elaps(st->last_discover) >= CLAUDE_DISCOVER_MS) {
        claude_redis_discover_step();
        st->last_discover = lv_tick_get();
    }

    if (st->last_poll == 0 || lv_tick_elaps(st->last_poll) >= CLAUDE_POLL_MS) {
        poll(st);
        st->last_poll = lv_tick_get();
    }
}

kd_mode_t *claude_mode_create(const char *id, const char *title) {
    kd_mode_t *m = calloc(1, sizeof(*m));
    claude_state_t *st = calloc(1, sizeof(*st));
    m->id = id;
    m->title = title;
    m->state = st;
    m->activate = activate;
    m->deactivate = NULL;
    m->tick = tick;
    return m;
}
