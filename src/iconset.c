/**
 * @file iconset.c
 * Pure model for the `icons` mode — set table, paging math, favourites set and
 * its bake-ready text format. No LVGL, no I/O. See iconset.h.
 */
#include "iconset.h"

#include <stdio.h>
#include <string.h>

/* ---- Set table -------------------------------------------------------- */

/* Nerd Font v3 glyph families. Ranges are declared generously (the mode filters
 * to glyphs the font actually contains). The two giant families are split into
 * `parts` evenly-sized sub-sets so no single set is unwieldy and "Pick Set"
 * steps through digestible chunks. Small friendly families come first. */
typedef struct {
    const char *name;
    uint32_t    start;
    uint32_t    end;
    int         parts; /* 1 = one set; >1 = split into "<name> N" sub-sets */
} icon_group_t;

static const icon_group_t GROUPS[] = {
    { "Font Logos",       0xF300,  0xF381,  1  },
    { "Devicons",         0xE700,  0xE7C5,  1  },
    { "Seti-UI",          0xE5FA,  0xE6B7,  1  },
    { "Weather",          0xE300,  0xE3EB,  1  },
    { "Codicons",         0xEA60,  0xEBEB,  1  },
    { "Octicons",         0xF400,  0xF533,  1  },
    { "Powerline",        0xE0A0,  0xE0D7,  1  },
    { "Pomicons",         0xE000,  0xE00D,  1  },
    { "FA Extension",     0xE200,  0xE2A9,  1  },
    { "Font Awesome",     0xF000,  0xF2E0,  2  },
    { "Material Design",  0xF0001, 0xF1AF0, 14 },
};
#define GROUP_COUNT ((int)(sizeof(GROUPS) / sizeof(GROUPS[0])))

int iconset_count(void) {
    int n = 0;
    for (int i = 0; i < GROUP_COUNT; i++)
        n += GROUPS[i].parts;
    return n;
}

/* Codepoint slice [start,end] for sub-set `part` of `parts` over [g0,g1]. */
static void slice(uint32_t g0, uint32_t g1, int part, int parts, uint32_t *s,
                  uint32_t *e) {
    uint32_t width = (g1 - g0 + 1) / (uint32_t)parts;
    *s = g0 + width * (uint32_t)part;
    *e = (part == parts - 1) ? g1 : (g0 + width * (uint32_t)(part + 1) - 1);
}

int iconset_at(int index, iconset_set_t *out) {
    if (index < 0 || !out)
        return -1;
    for (int i = 0; i < GROUP_COUNT; i++) {
        if (index < GROUPS[i].parts) {
            if (GROUPS[i].parts == 1) {
                snprintf(out->name, sizeof(out->name), "%s", GROUPS[i].name);
                out->start = GROUPS[i].start;
                out->end = GROUPS[i].end;
            } else {
                snprintf(out->name, sizeof(out->name), "%s %d", GROUPS[i].name,
                         index + 1);
                slice(GROUPS[i].start, GROUPS[i].end, index, GROUPS[i].parts,
                      &out->start, &out->end);
            }
            return 0;
        }
        index -= GROUPS[i].parts;
    }
    return -1;
}

int iconset_wrap(int index, int delta) {
    int n = iconset_count();
    if (n <= 0)
        return 0;
    /* Positive modulo regardless of delta sign. */
    long r = ((long)index + delta) % n;
    if (r < 0)
        r += n;
    return (int)r;
}

const char *iconset_name_for_cp(uint32_t cp) {
    for (int i = 0; i < GROUP_COUNT; i++)
        if (cp >= GROUPS[i].start && cp <= GROUPS[i].end)
            return GROUPS[i].name;
    return NULL;
}

/* ---- Paging ----------------------------------------------------------- */

int iconset_page_count(int present, int per_page) {
    if (per_page <= 0)
        return 1;
    if (present <= 0)
        return 1;
    return (present + per_page - 1) / per_page;
}

int iconset_clamp_page(int page, int present, int per_page) {
    int pages = iconset_page_count(present, per_page);
    if (page < 0)
        return 0;
    if (page >= pages)
        return pages - 1;
    return page;
}

/* ---- Favourites ------------------------------------------------------- */

void iconset_favs_clear(iconset_favs_t *f) {
    if (f)
        f->count = 0;
}

/* Binary search for cp; returns index if found, else -(insertion_point+1). */
static int fav_find(const iconset_favs_t *f, uint32_t cp) {
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->cp[mid] == cp)
            return mid;
        if (f->cp[mid] < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -(lo + 1);
}

int iconset_favs_contains(const iconset_favs_t *f, uint32_t cp) {
    if (!f || f->count == 0)
        return 0;
    return fav_find(f, cp) >= 0;
}

int iconset_favs_add(iconset_favs_t *f, uint32_t cp) {
    if (!f || f->count >= ICONSET_FAV_CAP)
        return 0;
    int pos = fav_find(f, cp);
    if (pos >= 0)
        return 0; /* already present */
    int ins = -(pos + 1);
    memmove(&f->cp[ins + 1], &f->cp[ins],
            (size_t)(f->count - ins) * sizeof(f->cp[0]));
    f->cp[ins] = cp;
    f->count++;
    return 1;
}

int iconset_favs_remove(iconset_favs_t *f, uint32_t cp) {
    if (!f || f->count == 0)
        return 0;
    int pos = fav_find(f, cp);
    if (pos < 0)
        return 0;
    memmove(&f->cp[pos], &f->cp[pos + 1],
            (size_t)(f->count - pos - 1) * sizeof(f->cp[0]));
    f->count--;
    return 1;
}

int iconset_favs_toggle(iconset_favs_t *f, uint32_t cp) {
    if (iconset_favs_contains(f, cp)) {
        iconset_favs_remove(f, cp);
        return 0;
    }
    return iconset_favs_add(f, cp); /* 1 if added, 0 if full */
}

int iconset_favs_count(const iconset_favs_t *f) {
    return f ? f->count : 0;
}

uint32_t iconset_favs_at(const iconset_favs_t *f, int i) {
    if (!f || i < 0 || i >= f->count)
        return 0;
    return f->cp[i];
}

/* ---- Favourites file format ------------------------------------------- */

/* Parse one hex codepoint token from `s`, tolerating a leading `0x`/`U+` and
 * surrounding space. Returns the value and sets *ok; ignores trailing junk. */
static uint32_t parse_cp(const char *s, int *ok) {
    *ok = 0;
    while (*s == ' ' || *s == '\t')
        s++;
    if ((s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ||
        (s[0] == 'U' && s[1] == '+') || (s[0] == 'u' && s[1] == '+'))
        s += 2;
    uint32_t v = 0;
    int digits = 0;
    for (; *s; s++) {
        int d;
        if (*s >= '0' && *s <= '9')
            d = *s - '0';
        else if (*s >= 'a' && *s <= 'f')
            d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F')
            d = *s - 'A' + 10;
        else
            break; /* stop at first non-hex (space, '#', newline, ...) */
        v = (v << 4) | (uint32_t)d;
        if (++digits > 8)
            return 0; /* absurdly long — reject */
    }
    if (digits == 0)
        return 0;
    *ok = 1;
    return v;
}

int iconset_favs_parse(iconset_favs_t *f, const char *text) {
    iconset_favs_clear(f);
    if (!text)
        return 0;
    const char *p = text;
    while (*p) {
        /* Isolate the current line [p, eol). */
        const char *eol = p;
        while (*eol && *eol != '\n')
            eol++;
        /* Skip leading whitespace to find the first significant char. */
        const char *q = p;
        while (q < eol && (*q == ' ' || *q == '\t'))
            q++;
        if (q < eol && *q != '#') {
            int ok = 0;
            uint32_t cp = parse_cp(q, &ok);
            if (ok && cp != 0)
                iconset_favs_add(f, cp);
        }
        p = (*eol == '\n') ? eol + 1 : eol;
    }
    return f->count;
}

size_t iconset_favs_format(const iconset_favs_t *f, char *buf, size_t cap) {
    if (!buf || cap == 0)
        return 0;
    buf[0] = '\0';
    if (!f)
        return 0;
    size_t len = 0;
    for (int i = 0; i < f->count; i++) {
        uint32_t cp = f->cp[i];
        const char *set = iconset_name_for_cp(cp);
        char line[64];
        int n = set ? snprintf(line, sizeof(line), "%04x  # %s\n", cp, set)
                    : snprintf(line, sizeof(line), "%04x\n", cp);
        if (n < 0)
            break;
        if (len + (size_t)n >= cap) /* would overflow (leave room for NUL) */
            break;
        memcpy(buf + len, line, (size_t)n);
        len += (size_t)n;
    }
    buf[len] = '\0';
    return len;
}
