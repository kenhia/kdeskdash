/**
 * @file bmp_write.h
 * Pure 24-bit BMP encoder for XRGB8888 pixel buffers. No LVGL, no Redis,
 * host-testable — the LVGL snapshot glue lives in screenshot.c.
 */
#ifndef KDESKDASH_BMP_WRITE_H
#define KDESKDASH_BMP_WRITE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/**
 * Encode an XRGB8888 buffer (little-endian B,G,R,X bytes per pixel — LVGL's
 * 32-bit native layout) as a bottom-up 24-bit BI_RGB BMP onto `f`.
 *
 * `stride` is the source row pitch in BYTES (>= w*4; snapshot buffers may pad).
 * Returns false on invalid arguments or a short write; the file may then be
 * partially written (caller owns cleanup).
 */
bool bmp_write_xrgb8888(FILE *f, const uint8_t *px, int w, int h, int stride);

#endif /* KDESKDASH_BMP_WRITE_H */
