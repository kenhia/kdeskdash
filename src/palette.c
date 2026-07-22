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

/* --- perceptual display order ---------------------------------------------
 * Sort key per color: (group, luma). Group 0 is "near-neutral" (chroma below
 * a threshold — the slate/grey chrome family); groups 1..6 are 60-degree hue
 * bins around the wheel. Within a group, luma ascends (dark to light). 60°
 * bins are deliberately coarse: the panel's blues span ~207-214° and must not
 * straddle a bin edge (30° bins split CPU_SKY from CODE_BLUE). */

#define NEUTRAL_CHROMA 48 /* of 255; STEEL_KEY (41) is chrome, SELECT_BLUE (115) is not */

static int luma_1000(uint32_t rgb) {
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return 299 * r + 587 * g + 114 * b;
}

static int sort_group(uint32_t rgb) {
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    int max = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int min = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int c = max - min;
    if (c < NEUTRAL_CHROMA)
        return 0;
    /* Hue in degrees (standard piecewise formula), then a 60-degree bin. */
    float h;
    if (max == r)
        h = 60.0f * (float)(g - b) / (float)c;
    else if (max == g)
        h = 60.0f * (2.0f + (float)(b - r) / (float)c);
    else
        h = 60.0f * (4.0f + (float)(r - g) / (float)c);
    if (h < 0)
        h += 360.0f;
    int bin = (int)(h / 60.0f);
    if (bin > 5)
        bin = 5;
    return 1 + bin;
}

void kd_pal_display_order(int *indices) {
    for (int i = 0; i < KD_PAL_COUNT; i++)
        indices[i] = i;
    /* Insertion sort — 30 entries, run once per mode activation. */
    for (int i = 1; i < KD_PAL_COUNT; i++) {
        int idx = indices[i];
        int g = sort_group(RGBS[idx]), l = luma_1000(RGBS[idx]);
        int j = i - 1;
        while (j >= 0) {
            int gj = sort_group(RGBS[indices[j]]);
            int lj = luma_1000(RGBS[indices[j]]);
            if (gj < g || (gj == g && lj <= l))
                break;
            indices[j + 1] = indices[j];
            j--;
        }
        indices[j + 1] = idx;
    }
}
