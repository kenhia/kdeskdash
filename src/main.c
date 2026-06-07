/**
 * @file main.c
 * kdeskdash entry point.
 *
 * Brings up the LVGL DRM display and evdev touch input, starts the mode shell
 * with its registered modes, and runs the LVGL main loop until SIGINT/SIGTERM,
 * then tears down cleanly.
 */
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "config.h"
#include "lvgl.h"
#include "modes/clock.h"
#include "modes/game_of_life.h"
#include "modes/menu.h"
#include "redis.h"
#include "shell.h"
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

    /* Optional global 180° flip: the panel mounts inverted in the case, so this
     * rotates render (and, via lv_indev, touch) for every mode. SPIKE: the
     * lv_linux_drm driver may not honor software rotation — verify on hardware
     * before relying on it (KMS rotate=180 + evdev calibration is the fallback). */
    if (cfg.rotate_180)
        lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_180);
    /* Mode shell: Game of Life and Clock are content modes; the Menu launcher
     * is the swipe-down target and startup default. */
    shell_init();
    shell_register_content_mode(
        game_of_life_mode_create("game_of_life", "Game of Life"));
    shell_register_content_mode(
        clock_mode_create("clock", "Clock"));
    shell_register_menu(menu_mode_create("menu", "Menu"));

    /* Optional Redis: remote control + last-mode persistence. Safe when absent.
     * Register the persistence hook before starting so the restored/initial
     * mode is written back, then restore the last active mode if one exists. */
    redis_init(cfg.redis_host, cfg.redis_port, cfg.redis_auth);
    shell_set_change_cb(redis_set_active_mode);
    char last_mode[64];
    const char *restore =
        redis_get_active_mode(last_mode, sizeof(last_mode)) ? last_mode : NULL;
    shell_start(restore);

    /* Capacitive touch via evdev (ILITEK, default /dev/input/event1).
     * Touch is optional: if it cannot be opened, the display still runs. */
    lv_indev_t *touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, cfg.touch_dev);
    if (!touch) {
        fprintf(stderr, "kdeskdash: warning — touch device %s unavailable; "
                        "running display-only\n", cfg.touch_dev);
    }

    printf("kdeskdash: running (DRM %s, touch %s, rotate_180 %s)\n",
           cfg.drm_dev, cfg.touch_dev, cfg.rotate_180 ? "on" : "off");

    /* Main loop */
    uint32_t last_poll = lv_tick_get();
    while (g_running) {
        shell_tick();
        /* Poll Redis ~once per second (remote control + reconnect). */
        if (lv_tick_elaps(last_poll) >= 1000) {
            redis_poll();
            last_poll = lv_tick_get();
        }
        uint32_t sleep_ms = lv_timer_handler();
        if (sleep_ms > 100)
            sleep_ms = 100;
        usleep(sleep_ms * 1000);
    }

    printf("\nkdeskdash: shutting down\n");
    redis_shutdown();
    lv_deinit();
    return 0;
}
