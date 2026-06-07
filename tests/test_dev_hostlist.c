/**
 * @file test_dev_hostlist.c
 * Host-only unit tests for the pure dev-mode host-selector list model.
 */
#include <stdio.h>
#include <string.h>

#include "modes/dev_hostlist.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* Build a fresh[][] snapshot from a NULL-terminated argument list. */
typedef char host_row[DEV_HOST_MAX];

static int fill(host_row *rows, const char *const *names) {
    int n = 0;
    for (; names[n] != NULL; n++)
        snprintf(rows[n], DEV_HOST_MAX, "%s", names[n]);
    return n;
}

static int idx_of(const dev_hostlist_t *l, const char *host) {
    for (int i = 0; i < l->count; i++)
        if (strcmp(l->entries[i].host, host) == 0)
            return i;
    return -1;
}

static void test_add_and_sort(void) {
    dev_hostlist_t l;
    dev_hostlist_init(&l);
    host_row rows[DEV_HOSTLIST_MAX];
    const char *names[] = {"kubs0", "kai", "zeta", NULL};
    int n = fill(rows, names);

    bool changed = dev_hostlist_merge(&l, rows, n, NULL, 0);
    check(changed, "first merge reports membership change");
    check(l.count == 3, "three hosts added");
    /* Lexicographic order regardless of input order. */
    check(strcmp(l.entries[0].host, "kai") == 0, "sorted[0] == kai");
    check(strcmp(l.entries[1].host, "kubs0") == 0, "sorted[1] == kubs0");
    check(strcmp(l.entries[2].host, "zeta") == 0, "sorted[2] == zeta");
    check(l.entries[0].online && l.entries[1].online && l.entries[2].online,
          "all online after first merge");
}

static void test_stable_no_change(void) {
    dev_hostlist_t l;
    dev_hostlist_init(&l);
    host_row rows[DEV_HOSTLIST_MAX];
    const char *names[] = {"kai", "kubs0", NULL};
    int n = fill(rows, names);

    dev_hostlist_merge(&l, rows, n, NULL, 0);
    bool changed = dev_hostlist_merge(&l, rows, n, NULL, 0);
    check(!changed, "identical snapshot reports no membership change");
    check(l.count == 2, "count stable");
}

static void test_dedup(void) {
    dev_hostlist_t l;
    dev_hostlist_init(&l);
    host_row rows[DEV_HOSTLIST_MAX];
    const char *names[] = {"kai", "kai", "kubs0", NULL};
    int n = fill(rows, names);

    dev_hostlist_merge(&l, rows, n, NULL, 0);
    check(l.count == 2, "duplicate fresh host de-duplicated");
}

static void test_debounce_survives_then_drops(void) {
    dev_hostlist_t l;
    dev_hostlist_init(&l);
    host_row rows[DEV_HOSTLIST_MAX];
    const char *both[] = {"kai", "kubs0", NULL};
    const char *one[] = {"kai", NULL};
    int n2 = fill(rows, both);
    dev_hostlist_merge(&l, rows, n2, NULL, 0);

    /* kubs0 absent: tick 1 (miss=1) and tick 2 (miss=2) keep it (offline). */
    host_row r1[DEV_HOSTLIST_MAX];
    int n1 = fill(r1, one);
    dev_hostlist_merge(&l, r1, n1, NULL, 0);
    check(idx_of(&l, "kubs0") >= 0, "absent host survives miss 1");
    int ki = idx_of(&l, "kubs0");
    check(ki >= 0 && !l.entries[ki].online, "absent host marked offline");

    dev_hostlist_merge(&l, r1, n1, NULL, 0);
    check(idx_of(&l, "kubs0") >= 0, "absent host survives miss 2");

    /* tick 3 (miss=3 > limit): dropped. */
    bool changed = dev_hostlist_merge(&l, r1, n1, NULL, 0);
    check(idx_of(&l, "kubs0") < 0, "absent host dropped after miss limit");
    check(changed, "drop reported as membership change");
    check(l.count == 1, "only kai remains");
}

static void test_reappear_resets_miss(void) {
    dev_hostlist_t l;
    dev_hostlist_init(&l);
    host_row rows[DEV_HOSTLIST_MAX];
    const char *both[] = {"kai", "kubs0", NULL};
    const char *one[] = {"kai", NULL};
    int n2 = fill(rows, both);
    int n1;
    host_row r1[DEV_HOSTLIST_MAX];
    n1 = fill(r1, one);

    dev_hostlist_merge(&l, rows, n2, NULL, 0);
    dev_hostlist_merge(&l, r1, n1, NULL, 0); /* kubs0 miss=1 */
    dev_hostlist_merge(&l, rows, n2, NULL, 0); /* kubs0 back */
    int ki = idx_of(&l, "kubs0");
    check(ki >= 0 && l.entries[ki].online && l.entries[ki].miss == 0,
          "reappeared host is online with miss reset");
}

static void test_keep_selected_never_drops(void) {
    dev_hostlist_t l;
    dev_hostlist_init(&l);
    host_row rows[DEV_HOSTLIST_MAX];
    const char *both[] = {"kai", "kubs0", NULL};
    const char *one[] = {"kai", NULL};
    int n2 = fill(rows, both);
    int n1;
    host_row r1[DEV_HOSTLIST_MAX];
    n1 = fill(r1, one);

    dev_hostlist_merge(&l, rows, n2, NULL, 0);

    /* kubs0 is the selected/kept host; absent for many ticks but never dropped. */
    const char *keep[] = {"kubs0"};
    for (int i = 0; i < 5; i++)
        dev_hostlist_merge(&l, r1, n1, keep, 1);

    int ki = idx_of(&l, "kubs0");
    check(ki >= 0, "kept host survives well past miss limit");
    check(ki >= 0 && !l.entries[ki].online, "kept host still marked offline");
}

static void test_cap(void) {
    dev_hostlist_t l;
    dev_hostlist_init(&l);
    host_row rows[DEV_HOSTLIST_MAX + 8];
    char names[DEV_HOSTLIST_MAX + 8][DEV_HOST_MAX];
    const char *ptrs[DEV_HOSTLIST_MAX + 9];
    int total = DEV_HOSTLIST_MAX + 8;
    for (int i = 0; i < total; i++) {
        snprintf(names[i], DEV_HOST_MAX, "host%02d", i);
        ptrs[i] = names[i];
    }
    ptrs[total] = NULL;
    int n = fill(rows, ptrs);
    dev_hostlist_merge(&l, rows, n, NULL, 0);
    check(l.count == DEV_HOSTLIST_MAX, "host count capped at DEV_HOSTLIST_MAX");
}

int main(void) {
    test_add_and_sort();
    test_stable_no_change();
    test_dedup();
    test_debounce_survives_then_drops();
    test_reappear_resets_miss();
    test_keep_selected_never_drops();
    test_cap();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_dev_hostlist: all passed\n");
    return 0;
}
