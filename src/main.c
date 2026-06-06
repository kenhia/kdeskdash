/**
 * @file main.c
 * kdeskdash entry point (pre-MVP).
 *
 * Brings up the LVGL DRM display and evdev touch input, draws the demo screen,
 * and runs the LVGL main loop until SIGINT/SIGTERM, then tears down cleanly.
 */
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "config.h"
#include "demo_screen.h"
#include "lvgl.h"
#include "src/drivers/display/drm/lv_linux_drm.h"
#include "src/drivers/evdev/lv_evdev.h"

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

int main(void) {
    /* Signal handling for clean teardown (R8) */
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    kdeskdash_config_t cfg;
    config_load(&cfg);

    lv_init();

    /* DRM/KMS display on the vc4 GPU (default /dev/dri/card1) */
    lv_display_t *disp = lv_linux_drm_create();
    if (!disp) {
        fprintf(stderr, "kdeskdash: failed to create DRM display\n");
        lv_deinit();
        return 1;
    }
    lv_linux_drm_set_file(disp, cfg.drm_dev, -1);

    /* Demo content — after display init so the active screen exists */
    demo_screen_create();

    /* Capacitive touch via evdev (ILITEK, default /dev/input/event1).
     * Touch is optional: if it cannot be opened, the display still runs. */
    lv_indev_t *touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, cfg.touch_dev);
    if (!touch) {
        fprintf(stderr, "kdeskdash: warning — touch device %s unavailable; "
                        "running display-only\n", cfg.touch_dev);
    }

    printf("kdeskdash: running (DRM %s, touch %s)\n", cfg.drm_dev, cfg.touch_dev);

    /* Main loop */
    while (g_running) {
        uint32_t sleep_ms = lv_timer_handler();
        if (sleep_ms > 100)
            sleep_ms = 100;
        usleep(sleep_ms * 1000);
    }

    printf("\nkdeskdash: shutting down\n");
    lv_deinit();
    return 0;
}
