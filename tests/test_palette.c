/**
 * @file test_palette.c
 * Host-only unit tests for the canonical named palette: table integrity
 * (unique, well-formed names; sane rgb values) and the name lookup.
 */
#include <stdio.h>
#include <string.h>

#include "palette.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void test_table_integrity(void) {
    int n = kd_pal_count();
    check(n == KD_PAL_COUNT, "count matches enum");
    check(n > 0, "palette is non-empty");

    for (int i = 0; i < n; i++) {
        const char *name = kd_pal_name(i);
        const char *usage = kd_pal_usage(i);
        char what[128];

        snprintf(what, sizeof(what), "entry %d has a name", i);
        check(name != NULL && name[0] != '\0', what);
        snprintf(what, sizeof(what), "entry %d has a usage note", i);
        check(usage != NULL && usage[0] != '\0', what);
        snprintf(what, sizeof(what), "%s rgb fits 24 bits",
                 name ? name : "?");
        check(kd_pal_rgb(i) <= 0xFFFFFF, what);

        /* Names are speakable identifiers: A-Z, 0-9, underscore. */
        for (const char *p = name; p && *p; p++) {
            int ok = (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                     *p == '_';
            if (!ok) {
                snprintf(what, sizeof(what), "%s charset ('%c')", name, *p);
                check(0, what);
                break;
            }
        }

        for (int j = i + 1; j < n; j++) {
            if (strcmp(name, kd_pal_name(j)) == 0) {
                snprintf(what, sizeof(what), "name %s is unique", name);
                check(0, what);
            }
            if (kd_pal_rgb(i) == kd_pal_rgb(j)) {
                snprintf(what, sizeof(what), "%s and %s share 0x%06X",
                         name, kd_pal_name(j), kd_pal_rgb(i));
                check(0, what);
            }
        }
    }
}

static void test_known_values(void) {
    int coral = kd_pal_find("CLAUDE_CORAL");
    check(coral >= 0, "CLAUDE_CORAL exists");
    check(kd_pal_rgb(coral) == 0xcf6b4a, "CLAUDE_CORAL hex");

    int bg = kd_pal_find("VOID");
    check(bg >= 0, "VOID exists");
    check(kd_pal_rgb(bg) == 0x05070d, "VOID hex");
}

static void test_find(void) {
    check(kd_pal_find("EDGE_TEAL") >= 0, "find exact");
    check(kd_pal_find("edge_teal") == kd_pal_find("EDGE_TEAL"),
          "find is case-insensitive");
    check(kd_pal_find("Edge_Teal") == kd_pal_find("EDGE_TEAL"),
          "find mixed case");
    check(kd_pal_find("BURNT_RADISH") == -1, "unknown name misses");
    check(kd_pal_find("EDGE_TEA") == -1, "prefix does not match");
    check(kd_pal_find("") == -1, "empty misses");
    check(kd_pal_find(NULL) == -1, "NULL is safe");
}

static void test_bounds(void) {
    check(kd_pal_name(-1) == NULL, "name(-1) NULL");
    check(kd_pal_name(kd_pal_count()) == NULL, "name(count) NULL");
    check(kd_pal_rgb(-1) == 0, "rgb(-1) 0");
    check(kd_pal_usage(9999) == NULL, "usage(9999) NULL");
}

int main(void) {
    test_table_integrity();
    test_known_values();
    test_find();
    test_bounds();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_palette: all passed\n");
    return 0;
}
