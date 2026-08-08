/**
 * @file modeset.c
 * KDESKDASH_MODES parser + the built-in default. See modeset.h for the grammar
 * and the roster's double duty.
 *
 * Everything here is bounded and allocation-free: the parse walks the spec with
 * explicit start/end pointers rather than mutating a copy, so it is safe to hand
 * a string straight out of getenv().
 */
#include "modeset.h"

#include <stdio.h>
#include <string.h>

/* The roster: every mode this build can create, in the order and grouping that
 * panels shipped with before per-device sets existed. This table is the single
 * source of truth for three things — the default set, the default grouping, and
 * which ids are legal — so a new mode is added here exactly once.
 *
 * Section names are drawn from this table too, which is why v1 accepts exactly
 * "fun" and "ops" without a separate list to keep in sync. */
static const struct {
    const char *section;
    const char *id;
} ROSTER[] = {
    {"fun", "game_of_life"},
    {"fun", "golz"},
    {"fun", "icons"},
    {"fun", "palette"},
    {"ops", "claude"},
    {"ops", "foreground"},
    {"ops", "launcher"},
    {"ops", "clock"},
    {"ops", "dev"},
    {"ops", "calc"},
};
#define ROSTER_N ((int)(sizeof(ROSTER) / sizeof(ROSTER[0])))

/* ---- small helpers ---------------------------------------------------- */

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Trim ASCII space from both ends of [*b, *e). Both bounds are exclusive-end. */
static void trim(const char **b, const char **e) {
    while (*b < *e && is_space(**b))
        (*b)++;
    while (*e > *b && is_space((*e)[-1]))
        (*e)--;
}

static bool span_eq(const char *b, const char *e, const char *s) {
    size_t n = (size_t)(e - b);
    return strlen(s) == n && strncmp(b, s, n) == 0;
}

/* Copy [b, e) into dst if it fits. False (and dst untouched) when it doesn't —
 * the caller warns and skips, which keeps a pathological spec from overrunning
 * a fixed buffer or silently truncating an id into a different valid one. */
static bool span_copy(char *dst, size_t cap, const char *b, const char *e) {
    size_t n = (size_t)(e - b);
    if (n == 0 || n >= cap)
        return false;
    memcpy(dst, b, n);
    dst[n] = '\0';
    return true;
}

static bool section_name_known(const char *b, const char *e) {
    for (int i = 0; i < ROSTER_N; i++)
        if (span_eq(b, e, ROSTER[i].section))
            return true;
    return false;
}

/* ---- building --------------------------------------------------------- */

/* Find an existing section by name, or -1. A spec that names the same section
 * twice merges into the first rather than creating a second. */
static int find_section(const modeset_t *ms, const char *b, const char *e) {
    for (int s = 0; s < ms->nsections; s++)
        if (span_eq(b, e, ms->sections[s].name))
            return s;
    return -1;
}

static bool has_id(const modeset_t *ms, const char *id) {
    for (int i = 0; i < ms->nids; i++)
        if (strcmp(ms->ids[i], id) == 0)
            return true;
    return false;
}

/* Append `id` to section `s`. Sections are contiguous slices of ids[], so this
 * shifts everything after the section's end down by one — an O(n) memmove over
 * at most 18 short strings, paid once per mode at startup. Keeping the slices
 * contiguous is what lets the swipe order and the tile order be the same list. */
static void insert_into_section(modeset_t *ms, int s, const char *id) {
    int at = ms->sections[s].first + ms->sections[s].count;

    for (int i = ms->nids; i > at; i--)
        memcpy(ms->ids[i], ms->ids[i - 1], MODESET_ID_MAX);
    memcpy(ms->ids[at], id, MODESET_ID_MAX);
    ms->nids++;

    ms->sections[s].count++;
    for (int o = 0; o < ms->nsections; o++)
        if (ms->sections[o].first > at)
            ms->sections[o].first++;
}

/* ---- default ---------------------------------------------------------- */

void modeset_default(modeset_t *ms) {
    memset(ms, 0, sizeof(*ms));
    for (int i = 0; i < ROSTER_N && ms->nids < MODESET_MAX_MODES; i++) {
        const char *name = ROSTER[i].section;
        const char *nend = name + strlen(name);
        int s = find_section(ms, name, nend);
        if (s < 0) {
            if (ms->nsections >= MODESET_MAX_SECTIONS)
                continue;
            s = ms->nsections++;
            snprintf(ms->sections[s].name, MODESET_NAME_MAX, "%s", name);
            ms->sections[s].first = ms->nids;
            ms->sections[s].count = 0;
        }
        insert_into_section(ms, s, ROSTER[i].id);
    }
}

/* ---- parse ------------------------------------------------------------ */

/* One "name:id,id,..." clause. Returns the number of modes it contributed. */
static int parse_section(modeset_t *ms, const char *b, const char *e) {
    const char *colon = memchr(b, ':', (size_t)(e - b));
    if (!colon) {
        fprintf(stderr, "kdeskdash: KDESKDASH_MODES: section \"%.*s\" has no "
                        "':' — skipped\n", (int)(e - b), b);
        return 0;
    }

    const char *nb = b, *ne = colon;
    trim(&nb, &ne);
    if (!section_name_known(nb, ne)) {
        fprintf(stderr, "kdeskdash: KDESKDASH_MODES: unknown section \"%.*s\" "
                        "— skipped\n", (int)(ne - nb), nb);
        return 0;
    }

    int s = find_section(ms, nb, ne);
    if (s < 0) {
        if (ms->nsections >= MODESET_MAX_SECTIONS) {
            fprintf(stderr, "kdeskdash: KDESKDASH_MODES: more than %d sections "
                            "— \"%.*s\" skipped\n", MODESET_MAX_SECTIONS,
                    (int)(ne - nb), nb);
            return 0;
        }
        s = ms->nsections++;
        span_copy(ms->sections[s].name, MODESET_NAME_MAX, nb, ne);
        ms->sections[s].first = ms->nids;
        ms->sections[s].count = 0;
    }

    int added = 0;
    const char *p = colon + 1;
    while (p <= e) {
        const char *comma = memchr(p, ',', (size_t)(e - p));
        const char *ib = p, *ie = comma ? comma : e;
        trim(&ib, &ie);

        if (ie > ib) {
            char id[MODESET_ID_MAX];
            if (!span_copy(id, sizeof(id), ib, ie)) {
                fprintf(stderr, "kdeskdash: KDESKDASH_MODES: id \"%.*s\" is too "
                                "long — skipped\n", (int)(ie - ib), ib);
            } else if (!modeset_id_known(id)) {
                fprintf(stderr, "kdeskdash: KDESKDASH_MODES: unknown mode \"%s\""
                                " — skipped\n", id);
            } else if (has_id(ms, id)) {
                fprintf(stderr, "kdeskdash: KDESKDASH_MODES: duplicate mode "
                                "\"%s\" — keeping the first\n", id);
            } else if (ms->nids >= MODESET_MAX_MODES) {
                fprintf(stderr, "kdeskdash: KDESKDASH_MODES: more than %d modes "
                                "— \"%s\" skipped\n", MODESET_MAX_MODES, id);
            } else {
                insert_into_section(ms, s, id);
                added++;
            }
        }

        if (!comma)
            break;
        p = comma + 1;
    }
    return added;
}

bool modeset_parse(modeset_t *ms, const char *spec) {
    if (!spec) {
        modeset_default(ms);
        return false;
    }

    const char *b = spec, *e = spec + strlen(spec);
    trim(&b, &e);
    if (b == e) {
        modeset_default(ms);
        return false;
    }

    memset(ms, 0, sizeof(*ms));
    const char *p = b;
    while (p <= e) {
        const char *semi = memchr(p, ';', (size_t)(e - p));
        const char *sb = p, *se = semi ? semi : e;
        trim(&sb, &se);
        if (se > sb)
            parse_section(ms, sb, se);
        if (!semi)
            break;
        p = semi + 1;
    }

    /* A spec that selects nothing would leave a blank panel with no way back
     * except an SSH session — exactly the failure this core exists to prevent. */
    if (ms->nids == 0) {
        fprintf(stderr, "kdeskdash: KDESKDASH_MODES selected no usable modes — "
                        "falling back to the full default set\n");
        modeset_default(ms);
        return false;
    }
    return true;
}

/* ---- queries ---------------------------------------------------------- */

bool modeset_id_known(const char *id) {
    if (!id || !*id)
        return false;
    for (int i = 0; i < ROSTER_N; i++)
        if (strcmp(id, ROSTER[i].id) == 0)
            return true;
    return false;
}

int modeset_count(const modeset_t *ms) {
    return ms->nids;
}

const char *modeset_at(const modeset_t *ms, int i) {
    if (i < 0 || i >= ms->nids)
        return NULL;
    return ms->ids[i];
}

bool modeset_enabled(const modeset_t *ms, const char *id) {
    return id != NULL && has_id(ms, id);
}

int modeset_section_count(const modeset_t *ms) {
    return ms->nsections;
}

const char *modeset_section_name(const modeset_t *ms, int s) {
    if (s < 0 || s >= ms->nsections)
        return NULL;
    return ms->sections[s].name;
}

int modeset_section_size(const modeset_t *ms, int s) {
    if (s < 0 || s >= ms->nsections)
        return 0;
    return ms->sections[s].count;
}

const char *modeset_section_at(const modeset_t *ms, int s, int i) {
    if (s < 0 || s >= ms->nsections)
        return NULL;
    if (i < 0 || i >= ms->sections[s].count)
        return NULL;
    return ms->ids[ms->sections[s].first + i];
}

const char *modeset_section_of(const modeset_t *ms, const char *id) {
    if (!id)
        return NULL;
    for (int s = 0; s < ms->nsections; s++)
        for (int i = 0; i < ms->sections[s].count; i++)
            if (strcmp(ms->ids[ms->sections[s].first + i], id) == 0)
                return ms->sections[s].name;
    return NULL;
}
