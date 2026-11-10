/**
 * @file modeset.h
 * Per-device mode selection: which content modes a panel registers, in what
 * order, and how they group in the Menu.
 *
 * Driven by one environment variable, parsed here as a pure core (no LVGL, no
 * Redis, no allocation) so the grammar is host-testable:
 *
 *     KDESKDASH_MODES="fun:game_of_life,golz;ops:claude,clock,calc"
 *     KDESKDASH_MODES="Play#EDGE_TEAL:golz,icons;Work:claude,clock;Tools:calc"
 *
 * Each `;`-separated clause is `name[#PALETTE]:id,id,...` (korg WI 668). The
 * name is the Menu header's text — any label, title-cased by the Menu, so the
 * legacy `fun`/`ops` still read "Fun"/"Ops". `#PALETTE` names a palette entry
 * (src/palette.h, case-insensitive) for the header; omitted or unknown means
 * CLAUDE_CORAL, the header colour before it was configurable — an unknown
 * colour warns and falls back, it never costs the section its modes. One to
 * three sections; the Menu splits its 18 tiles evenly between them.
 *
 * A section's list order is both the swipe-cycle order and the Menu tile order.
 * Unset (or unusable) falls back to the built-in default — the full set every
 * panel shipped before per-device sets existed — so a typo can never blank a
 * panel. Unknown ids, nameless or over-long section names, and a fourth
 * section warn on stderr and are skipped.
 *
 * The built-in default table in modeset.c doubles as the **roster**: the list of
 * ids this build knows how to create. Adding a mode means adding it there, to
 * main.c's creation table, and to CMakeLists — the Menu no longer needs editing.
 *
 * Section names are data, not API: the roster's `fun`/`ops` are only the
 * default grouping, and nothing outside the Menu reads a section name.
 */
#ifndef KDESKDASH_MODESET_H
#define KDESKDASH_MODESET_H

#include <stdbool.h>

/* The Menu shows 18 tiles in three rows, split evenly between one to three
 * sections (see modeset_section_capacity). */
#define MODESET_MAX_SECTIONS 3
#define MODESET_MAX_MODES    18
#define MODESET_MENU_ROWS    3
#define MODESET_ID_MAX       24
#define MODESET_NAME_MAX     24

typedef struct {
    char name[MODESET_NAME_MAX];
    int  color; /* KD_PAL_* index for the Menu header */
    int  first; /* index of this section's first id in modeset_t.ids */
    int  count;
} modeset_section_t;

typedef struct {
    /* Flat, declaration-ordered id list: the swipe-cycle order. Sections are
     * contiguous slices of it, so section order and mode order agree. */
    char ids[MODESET_MAX_MODES][MODESET_ID_MAX];
    int  nids;
    modeset_section_t sections[MODESET_MAX_SECTIONS];
    int  nsections;
} modeset_t;

/* Load the built-in default: every mode this build knows, in the order and
 * grouping panels used before per-device sets existed. */
void modeset_default(modeset_t *ms);

/* Parse `spec` (the KDESKDASH_MODES value) into `ms`. NULL, empty, blank, or a
 * spec that yields no usable modes falls back to modeset_default().
 *
 * Returns true when `spec` was actually used, false when the default was
 * substituted — so a caller can log which happened. Never leaves `ms` empty. */
bool modeset_parse(modeset_t *ms, const char *spec);

/* True if `id` is a mode this build can create (the roster). */
bool modeset_id_known(const char *id);

/* Selected modes in declaration order. modeset_at returns NULL out of range. */
int         modeset_count(const modeset_t *ms);
const char *modeset_at(const modeset_t *ms, int i);

/* True if `id` is in the set. NULL and unknown ids are false. */
bool modeset_enabled(const modeset_t *ms, const char *id);

/* Sections in declaration order. Names and per-section slices; out-of-range
 * indices return NULL / 0 rather than trapping. */
int         modeset_section_count(const modeset_t *ms);
const char *modeset_section_name(const modeset_t *ms, int s);
int         modeset_section_size(const modeset_t *ms, int s);
const char *modeset_section_at(const modeset_t *ms, int s, int i);

/* A section's header colour as a palette index (KD_PAL_*); -1 out of range. */
int         modeset_section_color(const modeset_t *ms, int s);

/* Menu tiles per section, derived from the section count: 18 for one, 9 for
 * two, 6 for three (0 for none). Columns are this over MODESET_MENU_ROWS. A
 * section with more modes than this still registers them — they stay on the
 * swipe cycle — but the Menu draws only the first `capacity`; the parse warns. */
int         modeset_section_capacity(const modeset_t *ms);

/* The name of the section holding `id`, or NULL if it is not in the set. */
const char *modeset_section_of(const modeset_t *ms, const char *id);

#endif /* KDESKDASH_MODESET_H */
