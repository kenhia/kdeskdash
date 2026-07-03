/**
 * @file claude_feed.c
 * Pure core for the `claude` mode feed. No Redis, no LVGL, host-testable.
 */
#include "claude_feed.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "telemetry_host.h" /* token contract shared with the telemetry feed */

/* ---------- key contract ---------- */

bool cf_key_parse(const char *key, size_t keylen,
                  char *host, size_t hostsz, char *sid, size_t sidsz) {
    if (!key || !host || !sid || hostsz == 0 || sidsz == 0)
        return false;

    static const size_t plen = sizeof(CF_KEY_PREFIX) - 1;
    if (keylen <= plen || memcmp(key, CF_KEY_PREFIX, plen) != 0)
        return false;

    const char *rest = key + plen;
    size_t restlen = keylen - plen;

    const char *colon = memchr(rest, ':', restlen);
    if (!colon)
        return false;

    size_t hlen = (size_t)(colon - rest);
    const char *sidp = colon + 1;
    size_t slen = restlen - hlen - 1;

    if (!telemetry_host_token_ok(rest, hlen) || !telemetry_host_token_ok(sidp, slen))
        return false; /* covers empty, oversize, charset, embedded ':' in sid */
    if (hlen + 1 > hostsz || slen + 1 > sidsz)
        return false; /* never truncate-and-use */

    memcpy(host, rest, hlen);
    host[hlen] = '\0';
    memcpy(sid, sidp, slen);
    sid[slen] = '\0';
    return true;
}

/* ---------- hash-field parsing ---------- */

/* Strict positive long long; rejects empty/garbage/trailing junk. */
static bool parse_ll(const char *s, long long *out) {
    if (!s || s[0] == '\0')
        return false;
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (end == s || *end != '\0' || v <= 0)
        return false;
    *out = v;
    return true;
}

static void copy_field(char *dst, size_t dstsz, const char *src) {
    snprintf(dst, dstsz, "%s", src ? src : "");
}

bool cf_session_from_fields(const char *host, const char *sid,
                            const char *const *fields, const char *const *values,
                            int nfields, cf_session_t *out) {
    if (!host || !sid || !out || nfields < 0)
        return false;
    if (!telemetry_host_token_ok(host, strlen(host)) ||
        !telemetry_host_token_ok(sid, strlen(sid)))
        return false;

    cf_session_t s;
    memset(&s, 0, sizeof(s));
    copy_field(s.host, sizeof(s.host), host);
    copy_field(s.sid, sizeof(s.sid), sid);

    bool have_status = false, have_ts = false;
    for (int i = 0; i < nfields; i++) {
        const char *f = fields[i];
        const char *v = values[i];
        if (!f || !v)
            continue;
        if (strcmp(f, "status") == 0) {
            if (strcmp(v, "working") == 0) {
                s.awaiting = false;
                have_status = true;
            } else if (strcmp(v, "awaiting") == 0) {
                s.awaiting = true;
                have_status = true;
            } else {
                return false; /* unknown status: distrust the whole record */
            }
        } else if (strcmp(f, "ts") == 0) {
            have_ts = parse_ll(v, &s.ts);
        } else if (strcmp(f, "started_ts") == 0) {
            long long st;
            if (parse_ll(v, &st))
                s.started_ts = st;
        } else if (strcmp(f, "project") == 0) {
            copy_field(s.project, sizeof(s.project), v);
        } else if (strcmp(f, "title") == 0) {
            copy_field(s.title, sizeof(s.title), v);
        } else if (strcmp(f, "model") == 0) {
            copy_field(s.model, sizeof(s.model), v);
        }
        /* cwd and unknown fields: ignored (not rendered) */
    }

    /* status+ts are the liveness contract; a hash without them is either a
     * statusline resurrection after SessionEnd or a foreign writer. */
    if (!have_status || !have_ts)
        return false;

    if (s.project[0] == '\0')
        copy_field(s.project, sizeof(s.project), "?");

    *out = s;
    return true;
}

/* ---------- display status + ordering ---------- */

cf_disp_t cf_display_status(bool awaiting, long long age_s) {
    if (age_s >= CF_STALE_S)
        return CF_DISP_STALE;
    if (awaiting)
        return CF_DISP_AWAITING; /* your turn — stays prominent until stale */
    if (age_s >= CF_IDLE_S)
        return CF_DISP_IDLE;
    return CF_DISP_WORKING;
}

const char *cf_disp_label(cf_disp_t d) {
    switch (d) {
    case CF_DISP_AWAITING: return "AWAITING INPUT";
    case CF_DISP_WORKING:  return "WORKING";
    case CF_DISP_IDLE:     return "IDLE";
    case CF_DISP_STALE:    return "STALE";
    }
    return "?";
}

static int session_cmp(const void *pa, const void *pb) {
    const cf_session_t *a = pa, *b = pb;
    if (a->disp != b->disp)
        return (a->disp < b->disp) ? -1 : 1;
    if (a->ts != b->ts)
        return (a->ts > b->ts) ? -1 : 1; /* most recent first */
    int h = strcmp(a->host, b->host);
    if (h != 0)
        return h;
    return strcmp(a->sid, b->sid);
}

void cf_sessions_refresh(cf_session_t *arr, int n, long long now) {
    if (!arr || n <= 0)
        return;
    for (int i = 0; i < n; i++)
        arr[i].disp = cf_display_status(arr[i].awaiting, now - arr[i].ts);
    qsort(arr, (size_t)n, sizeof(arr[0]), session_cmp);
}

/* ---------- limits ---------- */

/* Percentage: numeric, clamped to [0,100]; NaN/inf/garbage rejects. */
static bool parse_pct(const char *s, float *out) {
    if (!s || s[0] == '\0')
        return false;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || *end != '\0' || !isfinite(v))
        return false;
    if (v < 0.0)
        v = 0.0;
    if (v > 100.0)
        v = 100.0;
    *out = (float)v;
    return true;
}

bool cf_limits_from_fields(const char *const *fields, const char *const *values,
                           int nfields, cf_limits_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!fields || !values || nfields <= 0)
        return false;

    cf_limits_t l;
    memset(&l, 0, sizeof(l));
    bool have_five = false, have_seven = false;

    for (int i = 0; i < nfields; i++) {
        const char *f = fields[i];
        const char *v = values[i];
        if (!f || !v)
            continue;
        if (strcmp(f, "five_hour_pct") == 0)
            have_five = parse_pct(v, &l.five_pct);
        else if (strcmp(f, "seven_day_pct") == 0)
            have_seven = parse_pct(v, &l.seven_pct);
        else if (strcmp(f, "five_hour_resets_at") == 0)
            parse_ll(v, &l.five_reset);
        else if (strcmp(f, "seven_day_resets_at") == 0)
            parse_ll(v, &l.seven_reset);
        else if (strcmp(f, "updated_at") == 0)
            parse_ll(v, &l.updated_at);
    }

    if (!have_five || !have_seven)
        return false;

    l.valid = true;
    *out = l;
    return true;
}

/* ---------- recent records ---------- */

bool cf_recent_parse(const char *json, size_t len, cf_recent_t *out) {
    if (!json || len == 0 || !out)
        return false;
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root)
        return false;

    cf_recent_t r;
    memset(&r, 0, sizeof(r));
    bool ok = false;

    if (cJSON_IsObject(root)) {
        const cJSON *host = cJSON_GetObjectItemCaseSensitive(root, "host");
        const cJSON *proj = cJSON_GetObjectItemCaseSensitive(root, "project");
        if (cJSON_IsString(host) && host->valuestring &&
            telemetry_host_token_ok(host->valuestring, strlen(host->valuestring)) &&
            cJSON_IsString(proj) && proj->valuestring && proj->valuestring[0]) {
            copy_field(r.host, sizeof(r.host), host->valuestring);
            copy_field(r.project, sizeof(r.project), proj->valuestring);

            const cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "title");
            if (cJSON_IsString(title) && title->valuestring)
                copy_field(r.title, sizeof(r.title), title->valuestring);

            const cJSON *ended = cJSON_GetObjectItemCaseSensitive(root, "ended_ts");
            if (cJSON_IsNumber(ended) && isfinite(ended->valuedouble) &&
                ended->valuedouble > 0)
                r.ended_ts = (long long)ended->valuedouble;

            const cJSON *dur = cJSON_GetObjectItemCaseSensitive(root, "dur_s");
            if (cJSON_IsNumber(dur) && isfinite(dur->valuedouble) &&
                dur->valuedouble > 0)
                r.dur_s = (long long)dur->valuedouble;

            ok = true;
        }
    }

    cJSON_Delete(root);
    if (ok)
        *out = r;
    return ok;
}

/* ---------- formatting ---------- */

void cf_fmt_age(long long age_s, char *out, size_t outsz) {
    if (!out || outsz == 0)
        return;
    if (age_s < 0)
        age_s = 0;
    if (age_s < 60)
        snprintf(out, outsz, "%llds", age_s);
    else if (age_s < 3600)
        snprintf(out, outsz, "%lldm", age_s / 60);
    else if (age_s < 86400)
        snprintf(out, outsz, "%lldh", age_s / 3600);
    else
        snprintf(out, outsz, "%lldd", age_s / 86400);
}

void cf_fmt_reset(long long resets_at, long long now, char *out, size_t outsz) {
    if (!out || outsz == 0)
        return;
    if (resets_at <= 0) {
        snprintf(out, outsz, "--");
        return;
    }
    time_t t = (time_t)resets_at;
    struct tm tm;
    if (!localtime_r(&t, &tm)) {
        snprintf(out, outsz, "--");
        return;
    }
    /* Same-ish day (< 18h out): bare clock. Further: prefix the weekday. */
    if (resets_at - now < 18 * 3600)
        strftime(out, outsz, "%H:%M", &tm);
    else
        strftime(out, outsz, "%a %H:%M", &tm);
}
