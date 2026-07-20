/**
 * @file claude_feed.h
 * Pure core for the `claude` mode feed: session-key contract, hash-field
 * parsing, display-status derivation, attention-first ordering, usage-limits
 * snapshot, and the small formatting helpers the view renders verbatim.
 * No Redis, no LVGL, host-testable.
 *
 * Feed contract (written by publisher/claude-pub.sh, see
 * docs/plans/2026-07-02-001-feat-claude-mode-plan.md):
 *   claude:session:<host>:<sid>  hash — host/project/cwd/status/ts/started_ts
 *                                (+ model from the hook/transcript; title via
 *                                statusline, which runs in TUI sessions only)
 *   claude:limits                hash — five_hour_pct/..., seven_day_pct/...
 *   claude:recent                list of {"host","project","title","ended_ts","dur_s"}
 *
 * Both <host> and <sid> obey the telemetry host-token contract
 * ([A-Za-z0-9._-], 1..63 chars) — the same choke-point discipline as dev mode:
 * a key that fails the contract is skipped entirely.
 */
#ifndef KDESKDASH_CLAUDE_FEED_H
#define KDESKDASH_CLAUDE_FEED_H

#include <stdbool.h>
#include <stddef.h>

#define CF_KEY_PREFIX "claude:session:"

#define CF_HOST_MAX    64
#define CF_SID_MAX     64
#define CF_PROJECT_MAX 48
#define CF_TITLE_MAX   96
#define CF_MODEL_MAX   32

/* UI caps: rows the store tracks / recent entries surfaced. */
#define CF_SESSIONS_MAX 24
#define CF_RECENT_MAX   8

/* Display-status ladder (seconds since last lifecycle event). Hooks cannot
 * report a killed process, so trust the published status only while fresh. */
#define CF_IDLE_S  (15 * 60)
#define CF_STALE_S (40 * 60)

/* Usage arc switches to the warning treatment at this utilisation. */
#define CF_LIMITS_WARN_PCT 80.0f

/* Usage snapshot is treated as stale (greyed + badged) once its last publish is
 * this old. Limits only refresh while an interactive statusline session runs,
 * so a quiet fleet legitimately goes stale — say so rather than imply live. */
#define CF_LIMITS_STALE_S (60 * 60)

/* Published status, as written by publisher/claude-pub.sh. `blocked` means the
 * agent is sitting on an AskUserQuestion dialog and cannot proceed without the
 * user (PreToolUse sets it, PostToolUse clears it back to working). */
typedef enum {
    CF_ST_WORKING = 0,
    CF_ST_AWAITING,
    CF_ST_BLOCKED,
} cf_status_t;

/* Enum order is the sort rank: attention first. */
typedef enum {
    CF_DISP_BLOCKED = 0,  /* published `blocked`, still fresh — hard-blocked  */
    CF_DISP_AWAITING,     /* published `awaiting`, still fresh — your turn    */
    CF_DISP_WORKING,      /* published `working`, still fresh                 */
    CF_DISP_IDLE,         /* no event for CF_IDLE_S — probably parked         */
    CF_DISP_STALE,        /* no event for CF_STALE_S — probably killed        */
} cf_disp_t;

typedef struct {
    char host[CF_HOST_MAX];
    char sid[CF_SID_MAX];
    char project[CF_PROJECT_MAX];
    char title[CF_TITLE_MAX]; /* "" until statusline enriches (TUI only)    */
    char model[CF_MODEL_MAX]; /* "" until a hook reads it from the transcript*/
    long long ts;             /* unix s of last lifecycle event             */
    long long started_ts;     /* unix s of SessionStart (0 if unknown)      */
    cf_status_t status;       /* published status (working/awaiting/blocked)*/
    cf_disp_t disp;           /* derived by cf_sessions_refresh()           */
} cf_session_t;

typedef struct {
    char host[CF_HOST_MAX];
    char project[CF_PROJECT_MAX];
    char title[CF_TITLE_MAX];
    long long ended_ts;
    long long dur_s; /* 0 when unknown */
} cf_recent_t;

typedef struct {
    bool valid;
    float five_pct;          /* clamped to [0,100] */
    float seven_pct;
    long long five_reset;    /* unix s; 0 when unknown */
    long long seven_reset;
    long long updated_at;    /* unix s of last publish (drives "as of") */
} cf_limits_t;

/* Extract and validate <host> and <sid> from a `claude:session:<host>:<sid>`
 * key of length keylen. Anchored exactly; both tokens must pass the token
 * contract. On success copies both out (NUL-terminated) and returns true; on
 * any violation returns false and leaves the outputs untouched. */
bool cf_key_parse(const char *key, size_t keylen,
                  char *host, size_t hostsz, char *sid, size_t sidsz);

/* Build a session record from an HGETALL-style field/value list. `host`/`sid`
 * come from the (already validated) key. Requires a `status` field of exactly
 * "working", "awaiting" or "blocked" and a positive numeric `ts` — anything
 * else rejects
 * the record (guards the statusline-after-SessionEnd resurrection race, which
 * leaves a hash with no status). Unknown fields are ignored; missing project
 * falls back to "?". Returns true and fills *out on success. */
bool cf_session_from_fields(const char *host, const char *sid,
                            const char *const *fields, const char *const *values,
                            int nfields, cf_session_t *out);

/* Derived display status for a published state at `age_s` seconds old.
 * Negative ages (clock skew) count as fresh. */
cf_disp_t cf_display_status(cf_status_t status, long long age_s);

/* Fixed uppercase label for a display status ("BLOCKED ON YOU", ...). */
const char *cf_disp_label(cf_disp_t d);

/* Derive every record's disp for `now` and sort attention-first:
 * rank ascending (blocked, awaiting, working, idle, stale), then most-recent
 * ts first,
 * then host/sid lexicographic for a stable render order. */
void cf_sessions_refresh(cf_session_t *arr, int n, long long now);

/* Parse the claude:limits hash. Requires both *_pct fields numeric; percentages
 * clamp to [0,100]. Returns true and sets out->valid on success; on failure
 * returns false with *out zeroed (out->valid false). */
bool cf_limits_from_fields(const char *const *fields, const char *const *values,
                           int nfields, cf_limits_t *out);

/* True when the snapshot is valid but its last publish is at least
 * CF_LIMITS_STALE_S old. Negative ages (clock skew) are never stale. */
bool cf_limits_stale(const cf_limits_t *l, long long now);

/* Parse one claude:recent JSON record. Requires host+project strings passing
 * the same trust rules as rows (host token contract); title optional. */
bool cf_recent_parse(const char *json, size_t len, cf_recent_t *out);

/* Compact age string: "12s", "3m", "2h", "5d". Negative clamps to "0s". */
void cf_fmt_age(long long age_s, char *out, size_t outsz);

/* Wall-clock reset formatting (localtime): within ~18h -> "14:00", further out
 * -> "Tue 07:00", unknown (<= 0) -> "--". */
void cf_fmt_reset(long long resets_at, long long now, char *out, size_t outsz);

#endif /* KDESKDASH_CLAUDE_FEED_H */
