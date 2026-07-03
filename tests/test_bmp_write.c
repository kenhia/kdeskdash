/**
 * @file test_bmp_write.c
 * Host-only unit tests for the pure BMP encoder (no LVGL).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bmp_write.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int main(void) {
    /* 2x2 XRGB8888 (B,G,R,X bytes), 12-byte stride (one pad pixel per row):
     * top row: red, green — bottom row: blue, white. */
    const uint8_t px[] = {
        /* row 0 */ 0x00, 0x00, 0xff, 0x00,  0x00, 0xff, 0x00, 0x00,  0xaa, 0xaa, 0xaa, 0xaa,
        /* row 1 */ 0xff, 0x00, 0x00, 0x00,  0xff, 0xff, 0xff, 0x00,  0xbb, 0xbb, 0xbb, 0xbb,
    };

    FILE *f = tmpfile();
    check(f != NULL, "tmpfile");
    check(bmp_write_xrgb8888(f, px, 2, 2, 12), "encode 2x2");

    /* Expected: 54-byte header + 2 rows of (2*3 bytes + 2 pad) = 70 bytes. */
    long sz = ftell(f);
    check(sz == 70, "file size 70");

    uint8_t buf[70];
    rewind(f);
    check(fread(buf, 1, sizeof(buf), f) == sizeof(buf), "read back");
    fclose(f);

    check(buf[0] == 'B' && buf[1] == 'M', "magic");
    check(get32(buf + 2) == 70, "header file size");
    check(get32(buf + 10) == 54, "data offset");
    check(get32(buf + 18) == 2 && get32(buf + 22) == 2, "dimensions");
    check(buf[28] == 24, "24 bpp");
    check(get32(buf + 34) == 16, "image size (2 padded rows)");

    /* Bottom-up: file row 0 is source row 1 (blue, white). BMP stores B,G,R. */
    const uint8_t *r0 = buf + 54;
    check(r0[0] == 0xff && r0[1] == 0x00 && r0[2] == 0x00, "bottom-left blue");
    check(r0[3] == 0xff && r0[4] == 0xff && r0[5] == 0xff, "bottom-right white");
    const uint8_t *r1 = buf + 54 + 8;
    check(r1[0] == 0x00 && r1[1] == 0x00 && r1[2] == 0xff, "top-left red");
    check(r1[3] == 0x00 && r1[4] == 0xff && r1[5] == 0x00, "top-right green");

    /* Guards. */
    FILE *g = tmpfile();
    check(!bmp_write_xrgb8888(g, px, 0, 2, 12), "zero width rejects");
    check(!bmp_write_xrgb8888(g, px, 2, 2, 4), "undersized stride rejects");
    check(!bmp_write_xrgb8888(NULL, px, 2, 2, 12), "null file rejects");
    check(!bmp_write_xrgb8888(g, NULL, 2, 2, 12), "null pixels rejects");
    fclose(g);

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_bmp_write: all passed\n");
    return 0;
}
