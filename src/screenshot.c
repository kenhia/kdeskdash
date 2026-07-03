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

    bool ok = false;
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "kdeskdash: screenshot open %s failed\n", path);
    } else {
        ok = bmp_write_xrgb8888(f, buf->data, (int)buf->header.w,
                                (int)buf->header.h, (int)buf->header.stride);
        if (fclose(f) != 0)
            ok = false;
        if (!ok)
            fprintf(stderr, "kdeskdash: screenshot write %s failed\n", path);
        else
            printf("kdeskdash: screenshot saved to %s\n", path);
    }
    lv_draw_buf_destroy(buf);
    return ok;
}
