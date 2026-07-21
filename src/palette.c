/**
 * @file palette.c
 * Accessors over the KD_PALETTE X-macro table (see palette.h). Pure: no LVGL,
 * no allocation — three parallel static arrays generated from the one table.
 */
#include "palette.h"

#include <stddef.h>

#define KD_PAL_NAME_ROW(name, rgb, usage) #name,
#define KD_PAL_RGB_ROW(name, rgb, usage) rgb,
#define KD_PAL_USAGE_ROW(name, rgb, usage) usage,

static const char *NAMES[] = {KD_PALETTE(KD_PAL_NAME_ROW)};
static const uint32_t RGBS[] = {KD_PALETTE(KD_PAL_RGB_ROW)};
static const char *USAGES[] = {KD_PALETTE(KD_PAL_USAGE_ROW)};

int kd_pal_count(void) {
    return KD_PAL_COUNT;
}

static int in_range(int i) {
    return i >= 0 && i < KD_PAL_COUNT;
}

const char *kd_pal_name(int i) {
    return in_range(i) ? NAMES[i] : NULL;
}

uint32_t kd_pal_rgb(int i) {
    return in_range(i) ? RGBS[i] : 0;
}

const char *kd_pal_usage(int i) {
    return in_range(i) ? USAGES[i] : NULL;
}

/* Case-insensitive ASCII compare (no locale surprises, no strings.h). */
static int eq_nocase(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z')
            ca -= 'a' - 'A';
        if (cb >= 'a' && cb <= 'z')
            cb -= 'a' - 'A';
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

int kd_pal_find(const char *name) {
    if (!name)
        return -1;
    for (int i = 0; i < KD_PAL_COUNT; i++)
        if (eq_nocase(name, NAMES[i]))
            return i;
    return -1;
}
