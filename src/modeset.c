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
#include "palette.h"

#include <stdio.h>
#include <string.h>

/* The roster: every mode this build can create, in the order and grouping that
 * panels shipped with before per-device sets existed. This table is the single
 * source of truth for three things — the default set, the default grouping, and
 * which ids are legal — so a new mode is added here exactly once. Its section
 * names are only the default grouping: a spec may name sections anything. */
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

/* ---- building --------------------------------------------------------- */

#define DEFAULT_HEADER_COLOR KD_PAL_CLAUDE_CORAL

/* Open a new section at the end of ids[]. The caller has checked capacity and
 * that `name` fits. */
static int new_section(modeset_t *ms, const char *b, const char *e, int color) {
    int s = ms->nsections++;
    span_copy(ms->sections[s].name, MODESET_NAME_MAX, b, e);
    ms->sections[s].color = color;
    ms->sections[s].first = ms->nids;
    ms->sections[s].count = 0;
    return s;
}

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
            s = new_section(ms, name, nend, DEFAULT_HEADER_COLOR);
        }
        insert_into_section(ms, s, ROSTER[i].id);
    }
}

/* ---- parse ------------------------------------------------------------ */

/* The `#PALETTE` half of a clause head, [b, e) already trimmed. An unknown or
 * empty name warns and falls back: a header colour is cosmetic, and dropping a
 * section's modes over one would be the worst possible degradation. */
static int parse_color(const char *b, const char *e, const char *nb, const char *ne) {
    char name[32];
    int idx = -1;
    if (span_copy(name, sizeof(name), b, e))
        idx = kd_pal_find(name);
    if (idx < 0) {
        fprintf(stderr, "kdeskdash: KDESKDASH_MODES: section \"%.*s\": unknown "
                        "palette colour \"%.*s\" — using CLAUDE_CORAL\n",
                (int)(ne - nb), nb, (int)(e - b), b);
        return DEFAULT_HEADER_COLOR;
    }
    return idx;
}

/* One "name[#PALETTE]:id,id,..." clause. Returns the number of modes it
 * contributed. */
static int parse_section(modeset_t *ms, const char *b, const char *e) {
    const char *colon = memchr(b, ':', (size_t)(e - b));
    if (!colon) {
        fprintf(stderr, "kdeskdash: KDESKDASH_MODES: section \"%.*s\" has no "
                        "':' — skipped\n", (int)(e - b), b);
        return 0;
    }

    const char *hash = memchr(b, '#', (size_t)(colon - b));
    const char *nb = b, *ne = hash ? hash : colon;
    trim(&nb, &ne);
    if (ne == nb) {
        fprintf(stderr, "kdeskdash: KDESKDASH_MODES: section \"%.*s\" has no "
                        "name — skipped\n", (int)(e - b), b);
        return 0;
    }
    if ((size_t)(ne - nb) >= MODESET_NAME_MAX) {
        fprintf(stderr, "kdeskdash: KDESKDASH_MODES: section name \"%.*s\" is "
                        "longer than %d bytes — skipped\n", (int)(ne - nb), nb,
                MODESET_NAME_MAX - 1);
        return 0;
    }

    int color = DEFAULT_HEADER_COLOR;
    if (hash) {
        const char *cb = hash + 1, *ce = colon;
        trim(&cb, &ce);
        color = parse_color(cb, ce, nb, ne);
    }

    int s = find_section(ms, nb, ne);
    if (s < 0) {
        if (ms->nsections >= MODESET_MAX_SECTIONS) {
            fprintf(stderr, "kdeskdash: KDESKDASH_MODES: more than %d sections "
                            "— \"%.*s\" skipped\n", MODESET_MAX_SECTIONS,
                    (int)(ne - nb), nb);
            return 0;
        }
        s = new_section(ms, nb, ne, color);
    } else if (hash && color != ms->sections[s].color) {
        fprintf(stderr, "kdeskdash: KDESKDASH_MODES: section \"%s\" repeated "
                        "with another colour — keeping the first\n",
                ms->sections[s].name);
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

    int cap = modeset_section_capacity(ms);
    for (int s = 0; s < ms->nsections; s++)
        if (ms->sections[s].count > cap)
            fprintf(stderr, "kdeskdash: KDESKDASH_MODES: section \"%s\" has %d "
                            "modes; with %d section(s) the Menu shows %d — the "
                            "rest stay on the swipe cycle only\n",
                    ms->sections[s].name, ms->sections[s].count,
                    ms->nsections, cap);
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

int modeset_section_color(const modeset_t *ms, int s) {
    if (s < 0 || s >= ms->nsections)
        return -1;
    return ms->sections[s].color;
}

int modeset_section_capacity(const modeset_t *ms) {
    if (ms->nsections <= 0)
        return 0;
    return MODESET_MAX_MODES / ms->nsections;
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
