/**
 * @file registry.c
 * Pure content-mode index navigation (no LVGL dependency, host-testable).
 */
#include "registry.h"

int registry_wrap_index(int count, int current, int delta) {
    if (count <= 0)
        return 0;
    /* Two-step modulo keeps the result non-negative for negative deltas. */
    return (((current + delta) % count) + count) % count;
}
