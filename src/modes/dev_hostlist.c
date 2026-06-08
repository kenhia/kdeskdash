/**
 * @file dev_hostlist.c
 * Pure host-selector list model (see dev_hostlist.h).
 */
#include "modes/dev_hostlist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void dev_hostlist_init(dev_hostlist_t *l) {
    memset(l, 0, sizeof(*l));
}

static int find(const dev_hostlist_t *l, const char *host) {
    for (int i = 0; i < l->count; i++)
        if (strcmp(l->entries[i].host, host) == 0)
            return i;
    return -1;
}

static bool in_keep(const char *host, const char *const *keep, int keep_n) {
    for (int i = 0; i < keep_n; i++)
        if (keep[i] && keep[i][0] != '\0' && strcmp(keep[i], host) == 0)
            return true;
    return false;
}

static int cmp_entry(const void *a, const void *b) {
    const dev_host_entry_t *ea = a;
    const dev_host_entry_t *eb = b;
    return strcmp(ea->host, eb->host);
}

bool dev_hostlist_merge(dev_hostlist_t *l,
                        const char fresh[][DEV_HOST_MAX], int fresh_n,
                        const char *const *keep, int keep_n) {
    /* Snapshot pre-merge membership (already sorted from the last merge). */
    char before[DEV_HOSTLIST_MAX][DEV_HOST_MAX];
    int before_n = l->count;
    for (int i = 0; i < l->count; i++)
        snprintf(before[i], DEV_HOST_MAX, "%s", l->entries[i].host);

    /* Assume everything offline until proven present in this snapshot. */
    for (int i = 0; i < l->count; i++)
        l->entries[i].online = false;

    /* Fold in the fresh hosts (dedup; add new when there's room). */
    for (int i = 0; i < fresh_n; i++) {
        const char *h = fresh[i];
        if (!h || h[0] == '\0')
            continue;
        int idx = find(l, h);
        if (idx >= 0) {
            l->entries[idx].online = true;
            l->entries[idx].miss = 0;
        } else if (l->count < DEV_HOSTLIST_MAX) {
            dev_host_entry_t *e = &l->entries[l->count++];
            snprintf(e->host, DEV_HOST_MAX, "%s", h);
            e->online = true;
            e->miss = 0;
        }
    }

    /* Age the absent entries; drop those past the limit unless kept. */
    int w = 0;
    for (int r = 0; r < l->count; r++) {
        dev_host_entry_t *e = &l->entries[r];
        if (!e->online) {
            e->miss++;
            if (e->miss > DEV_HOSTLIST_MISS_LIMIT &&
                !in_keep(e->host, keep, keep_n))
                continue; /* drop */
        }
        if (w != r)
            l->entries[w] = *e;
        w++;
    }
    l->count = w;

    qsort(l->entries, (size_t)l->count, sizeof(l->entries[0]), cmp_entry);

    /* Membership changed? (set of host names, ignoring online/miss) */
    if (l->count != before_n)
        return true;
    for (int i = 0; i < l->count; i++)
        if (strcmp(l->entries[i].host, before[i]) != 0)
            return true;
    return false;
}
