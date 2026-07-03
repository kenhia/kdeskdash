/**
 * @file bmp_write.c
 * Pure 24-bit BMP encoder for XRGB8888 buffers. No LVGL, no Redis.
 */
#include "bmp_write.h"

#include <string.h>

/* Little-endian scalar writers (BMP is little-endian by definition). */
static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

bool bmp_write_xrgb8888(FILE *f, const uint8_t *px, int w, int h, int stride) {
    if (!f || !px || w <= 0 || h <= 0 || stride < w * 4)
        return false;

    /* Rows are padded to 4-byte boundaries in the file. */
    uint32_t row_bytes = (uint32_t)w * 3u;
    uint32_t row_pad = (4u - (row_bytes & 3u)) & 3u;
    uint32_t image_size = (row_bytes + row_pad) * (uint32_t)h;
    uint32_t data_off = 14u + 40u; /* BITMAPFILEHEADER + BITMAPINFOHEADER */

    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    put32(hdr + 2, data_off + image_size); /* file size */
    put32(hdr + 10, data_off);             /* pixel data offset */
    put32(hdr + 14, 40);                   /* BITMAPINFOHEADER size */
    put32(hdr + 18, (uint32_t)w);
    put32(hdr + 22, (uint32_t)h); /* positive height: bottom-up rows */
    put16(hdr + 26, 1);           /* planes */
    put16(hdr + 28, 24);          /* bits per pixel */
    put32(hdr + 34, image_size);
    put32(hdr + 38, 2835); /* ~72 DPI, conventional */
    put32(hdr + 42, 2835);
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
        return false;

    static const uint8_t pad[3] = {0, 0, 0};
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t *row = px + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; x++) {
            /* XRGB8888 little-endian bytes are B,G,R,X — BMP wants B,G,R. */
            if (fwrite(row + (size_t)x * 4u, 1, 3, f) != 3)
                return false;
        }
        if (row_pad && fwrite(pad, 1, row_pad, f) != row_pad)
            return false;
    }
    return true;
}
