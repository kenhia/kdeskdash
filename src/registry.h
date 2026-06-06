/**
 * @file registry.h
 * Pure content-mode index navigation (no LVGL dependency, host-testable).
 */
#ifndef KDESKDASH_REGISTRY_H
#define KDESKDASH_REGISTRY_H

/**
 * Wrap an index by `delta` steps within `count` slots.
 *
 * @param count   number of content modes (>= 0)
 * @param current current index
 * @param delta   step (+1 = next, -1 = prev; any integer works)
 * @return the wrapped index in [0, count), or 0 when count <= 0
 */
int registry_wrap_index(int count, int current, int delta);

#endif /* KDESKDASH_REGISTRY_H */
