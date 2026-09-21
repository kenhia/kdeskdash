/**
 * @file screenshot.c
 * LVGL glue for the device self-screenshot: lv_snapshot -> pure BMP encoder.
 */
#include "screenshot.h"

#include <stdio.h>

#include "bmp_write.h"
#include "lvgl.h"

bool screenshot_save(const char *path) {
    if (!path || path[0] == '\0')
        path = SCREENSHOT_DEFAULT_PATH;

    lv_draw_buf_t *buf =
        lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_XRGB8888);
    if (!buf) {
        fprintf(stderr, "kdeskdash: screenshot render failed\n");
        return false;
    }

    /* Atomic: <path>.tmp, then rename. The target therefore only ever exists
     * complete, which is what stops a consumer polling for a fresh mtime from
     * cat-ing a half-written 2.5 MB BMP (korg WI 2308). */
    bool ok = bmp_write_file_atomic(path, buf->data, (int)buf->header.w,
                                    (int)buf->header.h,
                                    (int)buf->header.stride);
    if (!ok)
        fprintf(stderr, "kdeskdash: screenshot write %s failed\n", path);
    else
        printf("kdeskdash: screenshot saved to %s\n", path);
    lv_draw_buf_destroy(buf);
    return ok;
}
