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

/**
 * The same encode, straight to `path`, ATOMICALLY: `<path>.tmp` is written,
 * flushed, fsynced and then rename()d over the target, so the target only ever
 * exists complete.
 *
 * This is the fix for korg WI 2308. A consumer cannot tell a finished
 * screenshot from one still being written: at 1920x440x3 the file is ~2.5 MB
 * and takes a visible moment to reach a Pi's SD card, while its mtime goes
 * fresh on the FIRST write. `scripts/kddss` polled the mtime and `cat`ed, and
 * in sprint 035 three shots of five came back truncated. The rename fixes it
 * for every consumer at once, including the deploy-panels skill's
 * `kddss deploy-$V` step, which is where it bit during a ship.
 *
 * Returns false on encode or I/O failure, having removed the temp file and
 * left any existing target untouched.
 */
bool bmp_write_file_atomic(const char *path, const uint8_t *px, int w, int h,
                           int stride);

#endif /* KDESKDASH_BMP_WRITE_H */
