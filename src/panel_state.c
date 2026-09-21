/**
 * @file panel_state.c
 * The durable panel state file: parse, serialize, load, atomic save.
 * Pure stdlib — see panel_state.h for the format and the whole-file rule.
 */
#include "panel_state.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The key table. Each row is either a counter (into a `long` member) or a
 * string (into a fixed buffer), so the parser and the serializer share one
 * description of the file and cannot drift apart. */
typedef enum { F_LONG, F_STR } field_kind_t;

typedef struct {
    const char  *key;
    field_kind_t kind;
    size_t       offset; /* into panel_state_t */
    size_t       size;   /* buffer size, strings only */
} field_t;

#define LONG_FIELD(k, m) \
    { k, F_LONG, offsetof(panel_state_t, m), 0 }
#define STR_FIELD(k, m) \
    { k, F_STR, offsetof(panel_state_t, m), sizeof(((panel_state_t *)0)->m) }

static const field_t FIELDS[] = {
    LONG_FIELD("golz.human_wins", golz_human_wins),
    LONG_FIELD("golz.zombie_wins", golz_zombie_wins),
    LONG_FIELD("golz.ties", golz_ties),
    LONG_FIELD("golz.gens_to_win", golz_gens_to_win),
    LONG_FIELD("golz.wins", golz_wins),
    STR_FIELD("calc.regs", calc_regs),
    STR_FIELD("dev.left", dev_left),
    STR_FIELD("dev.right", dev_right),
    STR_FIELD("active_mode", active_mode),
};
#define FIELD_COUNT (sizeof(FIELDS) / sizeof(FIELDS[0]))

static long *long_at(panel_state_t *st, const field_t *f) {
    return (long *)((char *)st + f->offset);
}

static char *str_at(panel_state_t *st, const field_t *f) {
    return (char *)st + f->offset;
}

void panel_state_defaults(panel_state_t *st) {
    if (!st)
        return;
    memset(st, 0, sizeof(*st));
    st->golz_human_wins = PANEL_STATE_UNSET;
    st->golz_zombie_wins = PANEL_STATE_UNSET;
    st->golz_ties = PANEL_STATE_UNSET;
    st->golz_gens_to_win = PANEL_STATE_UNSET;
    st->golz_wins = PANEL_STATE_UNSET;
}

/* Parse a whole non-negative decimal. Rejects an empty value, trailing junk,
 * a sign, and anything out of range — the value came off a file that may have
 * been truncated by a power cut, so "mostly a number" is not good enough. */
static bool parse_count(const char *val, size_t len, long *out) {
    if (len == 0 || len > 19)
        return false;
    char buf[24];
    memcpy(buf, val, len);
    buf[len] = '\0';
    errno = 0;
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (errno != 0 || end != buf + len || v < 0)
        return false;
    *out = v;
    return true;
}

bool panel_state_parse(const char *text, size_t len, panel_state_t *out) {
    if (!out)
        return false;
    panel_state_defaults(out);
    if (!text)
        return len == 0;

    /* Build into a scratch copy so a rejection late in the file cannot leave
     * the caller holding the fields that happened to come first. */
    panel_state_t st;
    panel_state_defaults(&st);

    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && text[i] != '\n')
            i++;
        size_t line_len = i - start;
        if (i < len)
            i++; /* step over the '\n' */
        const char *line = text + start;

        /* Blank and comment lines carry nothing. A line is blank only if it is
         * empty — leading whitespace is not part of the format we write, so
         * meeting it means this is not our file. */
        if (line_len == 0)
            continue;
        if (line[0] == '#')
            continue;

        const char *eq = memchr(line, '=', line_len);
        if (!eq)
            return false; /* not key=value: reject the file */
        size_t klen = (size_t)(eq - line);
        if (klen == 0)
            return false; /* empty key */
        const char *val = eq + 1;
        size_t vlen = line_len - klen - 1;

        const field_t *f = NULL;
        for (size_t k = 0; k < FIELD_COUNT; k++) {
            if (strlen(FIELDS[k].key) == klen &&
                memcmp(FIELDS[k].key, line, klen) == 0) {
                f = &FIELDS[k];
                break;
            }
        }
        /* The one thing that does not reject: a key this build does not know.
         * Rolling back to an older version must not brick the state file. */
        if (!f)
            continue;

        if (f->kind == F_LONG) {
            if (!parse_count(val, vlen, long_at(&st, f)))
                return false;
        } else {
            if (vlen == 0 || vlen >= f->size)
                return false; /* empty or over-length: never truncate into place */
            char *dst = str_at(&st, f);
            memcpy(dst, val, vlen);
            dst[vlen] = '\0';
        }
    }

    *out = st;
    return true;
}

size_t panel_state_serialize(const panel_state_t *st, char *out, size_t outsz) {
    if (!st || !out || outsz == 0)
        return 0;

    size_t used = 0;
    /* A tiny helper rather than repeated snprintf bookkeeping: every append
     * has to leave `used` truthful even when it did not fit. */
#define APPEND(...)                                                        \
    do {                                                                   \
        int n = snprintf(out + used, outsz - used, __VA_ARGS__);           \
        if (n < 0 || (size_t)n >= outsz - used)                            \
            return 0;                                                      \
        used += (size_t)n;                                                 \
    } while (0)

    APPEND("# kdeskdash panel state — written by the panel, read by the panel\n");
    for (size_t k = 0; k < FIELD_COUNT; k++) {
        const field_t *f = &FIELDS[k];
        if (f->kind == F_LONG) {
            long v = *long_at((panel_state_t *)st, f);
            if (v < 0)
                continue; /* unset: omit, so a round trip stays lossless */
            APPEND("%s=%ld\n", f->key, v);
        } else {
            const char *v = str_at((panel_state_t *)st, f);
            if (v[0] == '\0')
                continue;
            APPEND("%s=%s\n", f->key, v);
        }
    }
#undef APPEND
    return used;
}

panel_state_load_t panel_state_load(const char *path, panel_state_t *out) {
    if (!out)
        return PANEL_STATE_IO;
    panel_state_defaults(out);
    if (!path || path[0] == '\0')
        return PANEL_STATE_IO;

    FILE *f = fopen(path, "rb");
    if (!f)
        return (errno == ENOENT) ? PANEL_STATE_ABSENT : PANEL_STATE_IO;

    /* One byte of headroom over the cap, so "read exactly the cap" and "there
     * was more" are distinguishable without stat()ing separately. */
    static char buf[PANEL_STATE_FILE_MAX + 1];
    size_t n = fread(buf, 1, sizeof(buf), f);
    bool readerr = ferror(f) != 0;
    fclose(f);
    if (readerr)
        return PANEL_STATE_IO;
    if (n > PANEL_STATE_FILE_MAX)
        return PANEL_STATE_INVALID;

    return panel_state_parse(buf, n, out) ? PANEL_STATE_OK : PANEL_STATE_INVALID;
}

bool panel_state_save(const char *path, const panel_state_t *st) {
    if (!path || path[0] == '\0' || !st)
        return false;

    char text[PANEL_STATE_TEXT_MAX];
    size_t len = panel_state_serialize(st, text, sizeof(text));
    if (len == 0)
        return false;

    /* Same directory, so the rename is within one filesystem and therefore
     * atomic; a temp file elsewhere would turn this into a copy. */
    char tmp[PANEL_STATE_REGS_MAX + 256];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp))
        return false;

    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;
    bool ok = fwrite(text, 1, len, f) == len;
    /* fflush then fsync: the rename is only atomic with respect to a crash if
     * the bytes are on the device before it happens. */
    if (ok && fflush(f) != 0)
        ok = false;
    if (ok && fsync(fileno(f)) != 0)
        ok = false;
    if (fclose(f) != 0)
        ok = false;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp); /* leave no debris beside a state file we did not replace */
        return false;
    }
    return true;
}
