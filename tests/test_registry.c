/**
 * @file test_registry.c
 * Host-only unit tests for registry_wrap_index (no LVGL dependency).
 */
#include <stdio.h>

#include "registry.h"

static int failures;

static void check(int got, int want, const char *what) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %d, want %d\n", what, got, want);
        failures++;
    }
}

int main(void) {
    /* Two content modes: next and prev from either index yield the other. */
    check(registry_wrap_index(2, 0, +1), 1, "next from 0 of 2");
    check(registry_wrap_index(2, 1, +1), 0, "next from 1 of 2 wraps");
    check(registry_wrap_index(2, 0, -1), 1, "prev from 0 of 2 wraps");
    check(registry_wrap_index(2, 1, -1), 0, "prev from 1 of 2");

    /* Three modes: full forward and backward cycle. */
    check(registry_wrap_index(3, 2, +1), 0, "next from last of 3 wraps");
    check(registry_wrap_index(3, 0, -1), 2, "prev from first of 3 wraps");
    check(registry_wrap_index(3, 1, +1), 2, "next from middle of 3");

    /* Single mode: next/prev returns the same index. */
    check(registry_wrap_index(1, 0, +1), 0, "next of single");
    check(registry_wrap_index(1, 0, -1), 0, "prev of single");

    /* Degenerate counts return 0 rather than dividing by zero. */
    check(registry_wrap_index(0, 0, +1), 0, "empty registry");
    check(registry_wrap_index(-1, 0, +1), 0, "negative count");

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_registry: all passed\n");
    return 0;
}
