/**
 * @file iconset.h
 * Pure model for the `icons` Nerd Font browser mode — no LVGL, no I/O, so it is
 * host-testable. Owns three things:
 *
 *   1. The set table: the Nerd Font glyph families as name + codepoint range,
 *      with the two giant families (Font Awesome, Material Design) transparently
 *      split into evenly-sized sub-sets ("Material Design 3", ...).
 *   2. Paging arithmetic over a page of present glyphs (how many pages, clamp,
 *      wrap the set index). Which codepoints are actually present is decided by
 *      the mode (it probes the font); this module only does the math.
 *   3. The favourites set (a curated list of codepoints) and its on-disk text
 *      format — one lowercase-hex codepoint per line, so the file drops straight
 *      into `lv_font_conv -r` ranges for a future static bake.
 */
#ifndef KDESKDASH_ICONSET_H
#define KDESKDASH_ICONSET_H

#include <stddef.h>
#include <stdint.h>

/* ---- Set table -------------------------------------------------------- */

/* A resolved set: a display name and an inclusive codepoint range. The range
 * is declared generously; the mode filters to glyphs actually present in the
 * font, so gaps within a range are harmless. */
typedef struct {
    char     name[32];
    uint32_t start; /* inclusive */
    uint32_t end;   /* inclusive */
} iconset_set_t;

/* Number of browsable sets (giant families counted as their sub-sets). */
int iconset_count(void);

/* Resolve set `index` (0-based) into `out`. Returns 0 on success, -1 if the
 * index is out of range (out left untouched). */
int iconset_at(int index, iconset_set_t *out);

/* Wrap a set index by `delta` steps (for the Pick-Set button). Returns a valid
 * index in [0, iconset_count()); returns 0 when there are no sets. */
int iconset_wrap(int index, int delta);

/* Name of the set that owns `cp`, or NULL if no set's range contains it. Used
 * to annotate the favourites file. The returned pointer is a static base-group
 * name (no sub-set numbering) valid for the process lifetime. */
const char *iconset_name_for_cp(uint32_t cp);

/* ---- Paging ----------------------------------------------------------- */

/* Number of pages needed to show `present` glyphs `per_page` at a time.
 * Always >= 1 (an empty set still has one — blank — page). */
int iconset_page_count(int present, int per_page);

/* Clamp `page` into [0, page_count-1] for the given present/per_page. */
int iconset_clamp_page(int page, int present, int per_page);

/* ---- Favourites ------------------------------------------------------- */

#define ICONSET_FAV_CAP 512

/* Ordered (ascending) set of favourite codepoints, fixed capacity. */
typedef struct {
    uint32_t cp[ICONSET_FAV_CAP];
    int      count;
} iconset_favs_t;

void     iconset_favs_clear(iconset_favs_t *f);
int      iconset_favs_contains(const iconset_favs_t *f, uint32_t cp);
/* Add cp (kept sorted, deduped). Returns 1 if added, 0 if already present or
 * the set is full. */
int      iconset_favs_add(iconset_favs_t *f, uint32_t cp);
/* Remove cp. Returns 1 if it was present and removed, else 0. */
int      iconset_favs_remove(iconset_favs_t *f, uint32_t cp);
/* Toggle cp. Returns the new membership (1 = now a favourite, 0 = removed).
 * A toggle that would overflow a full set is a no-op returning 0. */
int      iconset_favs_toggle(iconset_favs_t *f, uint32_t cp);
int      iconset_favs_count(const iconset_favs_t *f);
/* Codepoint at ordered position `i`, or 0 if out of range. */
uint32_t iconset_favs_at(const iconset_favs_t *f, int i);

/* ---- Favourites file format ------------------------------------------- */

/* Parse favourites text into `f` (cleared first). Accepts one codepoint per
 * line as `f31b`, `0xf31b`, or `U+F31B` (case-insensitive); blank lines and
 * lines whose first non-space char is '#' are skipped; trailing text after the
 * hex token (e.g. a "# name" comment) is ignored; unparseable lines are
 * skipped, not fatal; duplicates collapse. Returns the number of favourites
 * loaded. */
int iconset_favs_parse(iconset_favs_t *f, const char *text);

/* Format `f` as text into `buf` (NUL-terminated, truncated to fit): one line
 * per favourite, `%04x` lowercase hex, a trailing "  # <set>" comment when the
 * codepoint belongs to a known set, ascending order. Returns the number of
 * bytes written (excluding the NUL), or 0 if buf/cap is unusable. */
size_t iconset_favs_format(const iconset_favs_t *f, char *buf, size_t cap);

#endif /* KDESKDASH_ICONSET_H */
