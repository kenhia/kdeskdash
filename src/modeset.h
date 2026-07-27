/**
 * @file modeset.h
 * Per-device mode selection: which content modes a panel registers, in what
 * order, and how they group in the Menu.
 *
 * Driven by one environment variable, parsed here as a pure core (no LVGL, no
 * Redis, no allocation) so the grammar is host-testable:
 *
 *     KDESKDASH_MODES="fun:game_of_life,golz;ops:claude,clock,calc"
 *
 * A section's list order is both the swipe-cycle order and the Menu tile order.
 * Unset (or unusable) falls back to the built-in default — the full set every
 * panel shipped before per-device sets existed — so a typo can never blank a
 * panel. Unknown ids and unknown section names warn on stderr and are skipped.
 *
 * The built-in default table in modeset.c doubles as the **roster**: the list of
 * ids this build knows how to create. Adding a mode means adding it there, to
 * main.c's creation table, and to CMakeLists — the Menu no longer needs editing.
 *
 * Section names are deliberately data, not API: v1 accepts exactly the two the
 * Menu draws (`fun`, `ops`), but nothing here bakes in "two groups with those
 * names", so making names/count configurable later (korg WI 668) does not
 * reshape any consumer.
 */
#ifndef KDESKDASH_MODESET_H
#define KDESKDASH_MODESET_H

#include <stdbool.h>

/* The Menu draws at most 9 tiles per section; three sections is the ceiling WI
 * 668 contemplates, and 18 tiles is the most it can show in any arrangement. */
#define MODESET_MAX_SECTIONS 3
#define MODESET_MAX_MODES    18
#define MODESET_ID_MAX       24
#define MODESET_NAME_MAX     16

typedef struct {
    char name[MODESET_NAME_MAX];
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

/* The name of the section holding `id`, or NULL if it is not in the set. */
const char *modeset_section_of(const modeset_t *ms, const char *id);

#endif /* KDESKDASH_MODESET_H */
