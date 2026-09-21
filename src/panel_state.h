/**
 * @file panel_state.h
 * The panel's durable state, as one file. Pure and stdlib-only: no Redis, no
 * LVGL, host-tested.
 *
 * Until sprint 039 this lived in a Redis server running on the panel's own
 * board — one whole daemon so a GoLZ score survived a restart. The file is the
 * authority now, and the loopback Redis is read exactly once, to migrate
 * (see panel_store.h).
 *
 * Format: flat `key=value` lines, `#` comments and blank lines ignored.
 * JSON was available (cJSON is vendored, libkdash parses it) and was not
 * taken: nothing reads this file but the panel that wrote it, and a flat
 * format keeps the core stdlib-only with countable failure modes.
 *
 *   # kdeskdash panel state
 *   golz.human_wins=41
 *   golz.gens_to_win=420
 *   calc.regs=0:1.4142135623730951 3:-1.5
 *   dev.left=kai
 *   active_mode=golz
 *
 * A malformed file is rejected WHOLE — the rule `calc:regs` already followed,
 * for the same reason: half-restored state is worse than none, because nobody
 * can tell which half they are looking at.
 *
 * With one deliberate exception. An **unknown key** is ignored, not rejected,
 * because naming an older version is how this project rolls back
 * (`just deploy <host> <old-version>`): a key a later build added must not
 * brick the state file on the way back down. A key this build knows, carrying
 * a value it cannot parse, still rejects the file.
 *
 * Absent fields are the normal case, not an error: a counter is "unset"
 * (PANEL_STATE_UNSET) and a string is empty, and the caller supplies its own
 * default — exactly the shape the Redis getters had.
 */
#ifndef KDESKDASH_PANEL_STATE_H
#define KDESKDASH_PANEL_STATE_H

#include <stdbool.h>
#include <stddef.h>

/* Default location. The systemd unit's StateDirectory=kdeskdash creates it and
 * makes it the one writable path under ProtectSystem=strict. */
#define PANEL_STATE_DEFAULT_PATH "/var/lib/kdeskdash/state"

/* Sentinel for a counter the file does not carry. Negative so it can never be
 * confused with a real count, and so `< 0` is the one test a caller needs. */
#define PANEL_STATE_UNSET (-1L)

#define PANEL_STATE_MODE_MAX 64  /* mode id; >= the shell's own buffer     */
#define PANEL_STATE_HOST_MAX 64  /* dev-mode host token; == DEV_HOST_MAX   */
#define PANEL_STATE_REGS_MAX 256 /* calc register line; > CALC_REGS_STR_MAX */

/* Refuse a file bigger than this rather than growing a buffer for whatever is
 * on disk. The largest legitimate file is well under 512 bytes. */
#define PANEL_STATE_FILE_MAX 4096

/* Enough for the serialized form of any valid state, plus slack. */
#define PANEL_STATE_TEXT_MAX 1024

typedef struct {
    long golz_human_wins;  /* golz.human_wins  */
    long golz_zombie_wins; /* golz.zombie_wins */
    long golz_ties;        /* golz.ties        */
    long golz_gens_to_win; /* golz.gens_to_win */
    long golz_wins;        /* golz.wins — legacy pre-machete counter, display-only */
    char calc_regs[PANEL_STATE_REGS_MAX];  /* calc.regs   */
    char dev_left[PANEL_STATE_HOST_MAX];   /* dev.left    */
    char dev_right[PANEL_STATE_HOST_MAX];  /* dev.right   */
    char active_mode[PANEL_STATE_MODE_MAX]; /* active_mode */
} panel_state_t;

/* How a load ended. ABSENT is deliberately distinct from INVALID: the one-time
 * migration from the legacy Redis fires on ABSENT only. A file that exists and
 * was rejected must NOT be re-migrated over — the old values are stale by
 * then, and quietly resurrecting them is the failure this distinction exists
 * to prevent. */
typedef enum {
    PANEL_STATE_OK = 0,
    PANEL_STATE_ABSENT,  /* no such file — first run                      */
    PANEL_STATE_INVALID, /* present, rejected whole (malformed or oversize) */
    PANEL_STATE_IO,      /* present but unreadable                        */
} panel_state_load_t;

/* Every counter unset, every string empty. Always call this first; the parser
 * does it for you. */
void panel_state_defaults(panel_state_t *st);

/* Parse `len` bytes of file text. Returns false and leaves `out` at defaults
 * when anything in the text is malformed — the whole-file rule above.
 * `text` need not be NUL-terminated. */
bool panel_state_parse(const char *text, size_t len, panel_state_t *out);

/* Serialize to `out` (NUL-terminated). Unset counters and empty strings are
 * omitted, so a fresh panel writes a nearly empty file and a round trip is
 * lossless. Returns the byte count written (excluding the NUL), or 0 if it
 * would not fit — 0 is always a failure, since the header line alone is
 * non-empty. */
size_t panel_state_serialize(const panel_state_t *st, char *out, size_t outsz);

/* Read and parse `path`. `out` is left at defaults on anything but OK. */
panel_state_load_t panel_state_load(const char *path, panel_state_t *out);

/* Write `path` atomically: serialize to `<path>.tmp`, fsync, rename over the
 * target. A reader therefore never sees a partial state file, and a crash
 * mid-write leaves the previous one intact. False on serialize or I/O failure
 * (the target is untouched in that case). */
bool panel_state_save(const char *path, const panel_state_t *st);

#endif /* KDESKDASH_PANEL_STATE_H */
