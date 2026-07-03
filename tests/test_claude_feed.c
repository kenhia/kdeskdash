/**
 * @file test_claude_feed.c
 * Host-only unit tests for the pure claude-feed core (no Redis, no LVGL).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "claude_feed.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void check_str(const char *got, const char *want, const char *what) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL: %s — got \"%s\", want \"%s\"\n", what, got, want);
        failures++;
    }
}

/* ---------- cf_key_parse ---------- */

static void key_ok(const char *key, const char *whost, const char *wsid) {
    char h[CF_HOST_MAX], s[CF_SID_MAX];
    if (!cf_key_parse(key, strlen(key), h, sizeof(h), s, sizeof(s))) {
        fprintf(stderr, "FAIL: expected accept for \"%s\"\n", key);
        failures++;
        return;
    }
    check_str(h, whost, key);
    check_str(s, wsid, key);
}

static void key_reject(const char *key) {
    char h[CF_HOST_MAX], s[CF_SID_MAX];
    memcpy(h, "SENTINEL", 9);
    if (cf_key_parse(key, strlen(key), h, sizeof(h), s, sizeof(s))) {
        fprintf(stderr, "FAIL: expected reject for \"%s\"\n", key);
        failures++;
    }
    if (memcmp(h, "SENTINEL", 9) != 0) {
        fprintf(stderr, "FAIL: \"%s\" rejected but clobbered out buffer\n", key);
        failures++;
    }
}

static void test_key_parse(void) {
    key_ok("claude:session:kai:5a7b8413-f4e2-4c39-a8e3-f57f59aeb9ac",
           "kai", "5a7b8413-f4e2-4c39-a8e3-f57f59aeb9ac");
    key_ok("claude:session:a:b", "a", "b");
    key_ok("claude:session:win-box_2.local:sid_1", "win-box_2.local", "sid_1");

    key_reject("claude:session:kai");        /* no sid segment */
    key_reject("claude:session::sid");       /* empty host */
    key_reject("claude:session:kai:");       /* empty sid */
    key_reject("claude:session:kai:a:b");    /* embedded colon in sid */
    key_reject("claude:limits");             /* different key entirely */
    key_reject("claude:session:ho st:sid");  /* charset */
    key_reject("claude:session:kai:s*d");    /* charset */
    key_reject("");
    key_reject("x");

    /* Oversized tokens reject; 63 chars accepted. */
    char big[80], key[256];
    memset(big, 'x', 64);
    big[64] = '\0';
    snprintf(key, sizeof(key), "claude:session:%s:sid", big);
    key_reject(key);
    snprintf(key, sizeof(key), "claude:session:kai:%s", big);
    key_reject(key);
    big[63] = '\0';
    snprintf(key, sizeof(key), "claude:session:kai:%s", big);
    key_ok(key, "kai", big);

    /* Undersized output buffers must reject, not truncate. */
    {
        char h[3], s[64];
        check(!cf_key_parse("claude:session:kai:sid", 22, h, sizeof(h), s, sizeof(s)),
              "small host buffer rejects");
    }
}

/* ---------- cf_session_from_fields ---------- */

static bool mk_session(const char *status, const char *ts, cf_session_t *out) {
    const char *f[] = {"host", "project", "cwd", "status", "ts", "started_ts"};
    const char *v[] = {"kai", "kdeskdash", "/home/ken/src/tools/kdeskdash",
                       status, ts, "1783000000"};
    return cf_session_from_fields("kai", "sid1", f, v, 6, out);
}

static void test_session_fields(void) {
    cf_session_t s;

    check(mk_session("working", "1783000100", &s), "working record parses");
    check(!s.awaiting, "working -> awaiting false");
    check(s.ts == 1783000100, "ts parsed");
    check(s.started_ts == 1783000000, "started_ts parsed");
    check_str(s.project, "kdeskdash", "project");
    check_str(s.title, "", "title empty until enriched");

    check(mk_session("awaiting", "1783000100", &s), "awaiting record parses");
    check(s.awaiting, "awaiting -> awaiting true");

    check(!mk_session("banana", "1783000100", &s), "unknown status rejects");
    check(!mk_session("working", "soon", &s), "non-numeric ts rejects");
    check(!mk_session("working", "-5", &s), "negative ts rejects");

    /* Missing status entirely (statusline resurrection race). */
    {
        const char *f[] = {"title", "model"};
        const char *v[] = {"Some Title", "Fable 5"};
        check(!cf_session_from_fields("kai", "sid1", f, v, 2, &s),
              "status-less hash rejects");
    }
    /* Missing ts likewise. */
    {
        const char *f[] = {"status"};
        const char *v[] = {"working"};
        check(!cf_session_from_fields("kai", "sid1", f, v, 1, &s),
              "ts-less hash rejects");
    }
    /* Missing project falls back to "?". */
    {
        const char *f[] = {"status", "ts"};
        const char *v[] = {"working", "17"};
        check(cf_session_from_fields("kai", "sid1", f, v, 2, &s), "minimal record");
        check_str(s.project, "?", "project fallback");
    }
    /* Enrichment fields carried through; oversize values truncate safely. */
    {
        char long_title[200];
        memset(long_title, 'T', sizeof(long_title) - 1);
        long_title[sizeof(long_title) - 1] = '\0';
        const char *f[] = {"status", "ts", "title", "model"};
        const char *v[] = {"awaiting", "17", long_title, "Fable 5"};
        check(cf_session_from_fields("kai", "sid1", f, v, 4, &s), "enriched record");
        check(strlen(s.title) == CF_TITLE_MAX - 1, "long title truncated");
        check_str(s.model, "Fable 5", "model");
    }
    /* Bad host/sid tokens reject even with clean fields. */
    {
        const char *f[] = {"status", "ts"};
        const char *v[] = {"working", "17"};
        check(!cf_session_from_fields("ho st", "sid1", f, v, 2, &s), "bad host token");
        check(!cf_session_from_fields("kai", "s:d", f, v, 2, &s), "bad sid token");
    }
}

/* ---------- display status + sort ---------- */

static void test_display_status(void) {
    check(cf_display_status(false, 5) == CF_DISP_WORKING, "fresh working");
    check(cf_display_status(true, 5) == CF_DISP_AWAITING, "fresh awaiting");
    check(cf_display_status(false, -30) == CF_DISP_WORKING, "clock skew is fresh");
    check(cf_display_status(false, CF_IDLE_S) == CF_DISP_IDLE, "idle at threshold");
    check(cf_display_status(true, CF_IDLE_S) == CF_DISP_AWAITING,
          "awaiting stays prominent past idle");
    check(cf_display_status(false, CF_STALE_S) == CF_DISP_STALE, "stale at threshold");
    check(cf_display_status(true, CF_STALE_S) == CF_DISP_STALE, "awaiting goes stale too");
    check_str(cf_disp_label(CF_DISP_AWAITING), "AWAITING INPUT", "awaiting label");
}

static void test_sort(void) {
    long long now = 1000000;
    cf_session_t a[5];
    memset(a, 0, sizeof(a));
    /* stale working (old), fresh working, awaiting, idle, fresher working */
    const struct { const char *sid; bool aw; long long ts; } in[5] = {
        {"stale",  false, now - CF_STALE_S - 5},
        {"work1",  false, now - 100},
        {"await",  true,  now - 300},
        {"idle",   false, now - CF_IDLE_S - 5},
        {"work2",  false, now - 10},
    };
    for (int i = 0; i < 5; i++) {
        snprintf(a[i].host, sizeof(a[i].host), "kai");
        snprintf(a[i].sid, sizeof(a[i].sid), "%s", in[i].sid);
        a[i].awaiting = in[i].aw;
        a[i].ts = in[i].ts;
    }
    cf_sessions_refresh(a, 5, now);
    check_str(a[0].sid, "await", "awaiting first");
    check_str(a[1].sid, "work2", "freshest working second");
    check_str(a[2].sid, "work1", "older working third");
    check_str(a[3].sid, "idle", "idle fourth");
    check_str(a[4].sid, "stale", "stale last");
    check(a[0].disp == CF_DISP_AWAITING && a[4].disp == CF_DISP_STALE,
          "disp derived during refresh");
}

/* ---------- limits ---------- */

static void test_limits(void) {
    cf_limits_t l;
    const char *f[] = {"five_hour_pct", "five_hour_resets_at",
                       "seven_day_pct", "seven_day_resets_at",
                       "updated_at", "host"};
    const char *v[] = {"11", "1783048800", "5.5", "1783598400", "1783035734", "kai"};
    check(cf_limits_from_fields(f, v, 6, &l), "limits parse");
    check(l.valid, "limits valid");
    check(l.five_pct == 11.0f, "five pct");
    check(l.seven_pct > 5.4f && l.seven_pct < 5.6f, "seven pct fractional");
    check(l.five_reset == 1783048800, "five reset");
    check(l.updated_at == 1783035734, "updated_at");

    /* Clamping and rejects. */
    const char *v2[] = {"150", "0", "-3", "0", "0", "kai"};
    check(cf_limits_from_fields(f, v2, 6, &l), "clamped limits parse");
    check(l.five_pct == 100.0f && l.seven_pct == 0.0f, "pct clamped to [0,100]");

    const char *v3[] = {"nope", "1", "5", "1", "1", "kai"};
    check(!cf_limits_from_fields(f, v3, 6, &l) && !l.valid, "garbage pct rejects");

    const char *f4[] = {"five_hour_pct"};
    const char *v4[] = {"11"};
    check(!cf_limits_from_fields(f4, v4, 1, &l), "missing seven_day rejects");
    check(!cf_limits_from_fields(NULL, NULL, 0, &l), "empty hash rejects");
}

/* ---------- recent ---------- */

static void test_recent(void) {
    cf_recent_t r;
    const char *good =
        "{\"host\":\"kai\",\"project\":\"scratchpad\",\"title\":\"Fix the thing\","
        "\"ended_ts\":1783035771,\"dur_s\":50}";
    check(cf_recent_parse(good, strlen(good), &r), "recent parses");
    check_str(r.host, "kai", "recent host");
    check_str(r.project, "scratchpad", "recent project");
    check_str(r.title, "Fix the thing", "recent title");
    check(r.ended_ts == 1783035771 && r.dur_s == 50, "recent numbers");

    const char *no_title = "{\"host\":\"kai\",\"project\":\"p\",\"ended_ts\":1,\"dur_s\":0}";
    check(cf_recent_parse(no_title, strlen(no_title), &r), "title optional");
    check_str(r.title, "", "empty title");
    check(r.dur_s == 0, "zero duration allowed");

    const char *bad_host = "{\"host\":\"h h\",\"project\":\"p\"}";
    check(!cf_recent_parse(bad_host, strlen(bad_host), &r), "bad host token rejects");
    const char *no_proj = "{\"host\":\"kai\"}";
    check(!cf_recent_parse(no_proj, strlen(no_proj), &r), "missing project rejects");
    check(!cf_recent_parse("not json", 8, &r), "garbage rejects");
    check(!cf_recent_parse("[1,2]", 5, &r), "non-object rejects");
}

/* ---------- formatting ---------- */

static void test_fmt(void) {
    char buf[32];
    cf_fmt_age(12, buf, sizeof(buf));   check_str(buf, "12s", "age 12s");
    cf_fmt_age(185, buf, sizeof(buf));  check_str(buf, "3m", "age 3m");
    cf_fmt_age(7200, buf, sizeof(buf)); check_str(buf, "2h", "age 2h");
    cf_fmt_age(200000, buf, sizeof(buf)); check_str(buf, "2d", "age 2d");
    cf_fmt_age(-5, buf, sizeof(buf));   check_str(buf, "0s", "negative clamps");

    /* Reset formatting is TZ-dependent: pin UTC for determinism. */
    setenv("TZ", "UTC", 1);
    tzset();
    long long now = 1783036800; /* 2026-07-03 00:00:00 UTC */
    cf_fmt_reset(now + 2 * 3600, now, buf, sizeof(buf));
    check_str(buf, "02:00", "near reset is bare clock");
    cf_fmt_reset(now + 30 * 3600, now, buf, sizeof(buf));
    check_str(buf, "Sat 06:00", "far reset carries weekday");
    cf_fmt_reset(0, now, buf, sizeof(buf));
    check_str(buf, "--", "unknown reset");
}

int main(void) {
    test_key_parse();
    test_session_fields();
    test_display_status();
    test_sort();
    test_limits();
    test_recent();
    test_fmt();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_claude_feed: all passed\n");
    return 0;
}
