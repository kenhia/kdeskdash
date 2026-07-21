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

#include "claude_redis.h"
#include "config.h"
#include "kvscf_redis.h"
#include "lvgl.h"
#include "modes/calc.h"
#include "modes/claude.h"
#include "modes/clock.h"
#include "modes/dev.h"
#include "modes/foreground.h"
#include "modes/game_of_life.h"
#include "modes/golz.h"
#include "modes/icons.h"
#include "modes/menu.h"
#include "modes/palette.h"
#include "redis.h"
#include "shell.h"
#include "telemetry.h"
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

    /* Optional global 180° flip: the panel mounts inverted in the case.
     * SPIKE RESULT (2026-06-07): lv_display_set_rotation() does NOT work here —
     * the lv_linux_drm flush ignores disp->rotation (display stays unrotated)
     * while lv_indev still transforms touch, which kills touch. So the toggle is
     * parsed but the rotation is NOT applied via the LVGL display rotation API.
     * A working path (driver-side lv_draw_sw_rotate / DRM plane rotation property,
     * or physical mounting) is pending the Unit 1 regroup — see the plan. */
    if (cfg.rotate_180) {
        fprintf(stderr, "kdeskdash: KDESKDASH_ROTATE_180 set, but software "
                        "rotation is not yet supported on this DRM driver — "
                        "ignoring (see plan Unit 1).\n");
    }
    /* Mode shell: Game of Life and Clock are content modes; the Menu launcher
     * is the swipe-down target and startup default. */
    shell_init();
    shell_register_content_mode(
        game_of_life_mode_create("game_of_life", "Game of Life"));
    shell_register_content_mode(
        golz_mode_create("golz", "GoLZ"));
    shell_register_content_mode(
        clock_mode_create("clock", "Clock"));
    shell_register_content_mode(
        dev_mode_create("dev", "Dev"));
    shell_register_content_mode(
        claude_mode_create("claude", "Claude"));
    shell_register_content_mode(
        icons_mode_create("icons", "Icons", cfg.icons_ttf_path,
                          cfg.icons_favorites_path));
    shell_register_content_mode(
        foreground_mode_create("foreground", "Remote", cfg.icons_ttf_path));
    shell_register_content_mode(
        calc_mode_create("calc", "Calc"));
    shell_register_content_mode(
        palette_mode_create("palette", "Palette"));
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

    /* Telemetry source (kpidash host metrics). Lazy connect on its own handle:
     * a down/slow endpoint never stalls boot or the control path. */
    telemetry_init(cfg.telemetry_redis_host, cfg.telemetry_redis_port,
                   cfg.telemetry_redis_auth);

    /* Claude feed (agent activity + usage limits): third independent handle,
     * localhost instance on this Pi by default. */
    claude_redis_init(cfg.claude_redis_host, cfg.claude_redis_port,
                      cfg.claude_redis_auth);

    /* kvscf window feed (foreground mode): reads/publishes on the same 6380 data
     * instance as the claude feed, but on its own handle for failure isolation.
     * The focus token comes from KVSCF_TOKEN (empty -> focusing disabled). */
    kvscf_redis_init(cfg.claude_redis_host, cfg.claude_redis_port,
                     cfg.claude_redis_auth, cfg.kvscf_token);

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
    telemetry_shutdown();
    claude_redis_shutdown();
    kvscf_redis_shutdown();
    lv_deinit();
    return 0;
}
