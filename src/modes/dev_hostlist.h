/**
 * @file dev_hostlist.h
 * Pure, host-testable model for the dev-mode host selector list (no LVGL, no
 * Redis). Maintains a deterministically-ordered (lexicographic) set of known
 * hosts, debounces brief drop/re-add churn from the SCAN discovery TTL, and
 * never drops "kept" hosts (the selected one, or current L/R assignments) so a
 * vanished-but-selected host stays visible as offline.
 */
#ifndef KDESKDASH_DEV_HOSTLIST_H
#define KDESKDASH_DEV_HOSTLIST_H

#include <stdbool.h>

#include "dev_telemetry.h" /* DEV_HOST_MAX */

#define DEV_HOSTLIST_MAX 16
/* Tolerate this many consecutive discovery ticks of a host being absent before
 * dropping it (a publisher stall just past the ~5 s TTL drops then re-adds). */
#define DEV_HOSTLIST_MISS_LIMIT 2

typedef struct {
    char host[DEV_HOST_MAX];
    int  miss;   /* consecutive discovery ticks missing (0 == seen this tick) */
    bool online; /* present in the most recent discovery snapshot */
} dev_host_entry_t;

typedef struct {
    dev_host_entry_t entries[DEV_HOSTLIST_MAX];
    int count;
} dev_hostlist_t;

/* Reset to empty. */
void dev_hostlist_init(dev_hostlist_t *l);

/**
 * Merge a fresh discovery snapshot (`fresh`, `fresh_n` rows of DEV_HOST_MAX).
 * Hosts in `keep` (each a NUL-terminated string; empty strings ignored) are
 * never dropped even when absent past the miss limit — they remain as offline
 * entries. After merging, entries are sorted lexicographically.
 *
 * Returns true if the visible host set (membership, ignoring online/miss state)
 * changed — i.e. the caller should rebuild row widgets. Returns false when only
 * online/miss state changed (caller need only repaint).
 */
bool dev_hostlist_merge(dev_hostlist_t *l,
                        const char fresh[][DEV_HOST_MAX], int fresh_n,
                        const char *const *keep, int keep_n);

#endif /* KDESKDASH_DEV_HOSTLIST_H */
