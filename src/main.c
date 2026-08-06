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
#include <string.h>
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
#include "modeset.h"
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

/* id -> constructor. This is the build's mode roster on the creation side; the
 * *selection* side (which ids exist, their default order and grouping) lives in
 * modeset.c. Adding a mode means one line there, one case here, and its source
 * in CMakeLists.
 *
 * A dispatch rather than a `{id, title, fn}` table because the constructors do
 * not share a signature — icons and foreground need paths from cfg — and three
 * adapter shims to force uniformity would cost more than they'd save.
 *
 * Returns NULL for an id this build cannot create, which modeset's roster
 * already rules out; the caller warns rather than trusting the two to agree. */
static kd_mode_t *create_mode(const char *id, const kdeskdash_config_t *cfg) {
    if (strcmp(id, "game_of_life") == 0)
        return game_of_life_mode_create("game_of_life", "Game of Life");
    if (strcmp(id, "golz") == 0)
        return golz_mode_create("golz", "GoLZ");
    if (strcmp(id, "clock") == 0)
        return clock_mode_create("clock", "Clock");
    if (strcmp(id, "dev") == 0)
        return dev_mode_create("dev", "Dev");
    if (strcmp(id, "claude") == 0)
        return claude_mode_create("claude", "Claude");
    if (strcmp(id, "icons") == 0)
        return icons_mode_create("icons", "Icons", cfg->icons_ttf_path,
                                 cfg->icons_favorites_path);
    if (strcmp(id, "foreground") == 0)
        return foreground_mode_create("foreground", "Remote",
                                      cfg->icons_ttf_path);
    if (strcmp(id, "calc") == 0)
        return calc_mode_create("calc", "Calc");
    if (strcmp(id, "palette") == 0)
        return palette_mode_create("palette", "Palette");
    return NULL;
}

/* Set by the build recipe (-DKD_VERSION); see scripts/version.sh. */
#ifndef KD_VERSION
#define KD_VERSION "unknown"
#endif

int main(int argc, char **argv) {
    /* --version before anything else touches hardware. A deploy asks the
     * *installed* binary on the board what it is, which is the only way to
     * prove the push landed — a running service answers a health check just
     * as well with yesterday's build. That means this has to work over ssh on
     * a Pi whose panel is already owned by the running instance, so it must
     * return before DRM master or evdev is claimed. */
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("kdeskdash %s\n", KD_VERSION);
        return 0;
    }

    /* Under systemd stdout is a pipe, so glibc block-buffers it: every startup
     * printf sat in a 4KB buffer until shutdown, which made `journalctl -u
     * kdeskdash` look silent on a running service. Line-buffer so diagnostics
     * land when they happen, matching stderr's unbuffered warnings. */
    setvbuf(stdout, NULL, _IOLBF, 0);

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
    /* Mode shell. Which content modes this panel registers, in what order and
     * under which Menu section, comes from KDESKDASH_MODES via the modeset core
     * — unset means the full built-in set. Static because the Menu keeps a
     * pointer to it for the lifetime of the program. The Menu launcher itself is
     * always registered: it is the swipe-down target and startup default. */
    static modeset_t modes;
    if (modeset_parse(&modes, cfg.modes_spec))
        printf("kdeskdash: mode set from KDESKDASH_MODES (%d modes)\n",
               modeset_count(&modes));

    shell_init();
    for (int i = 0; i < modeset_count(&modes); i++) {
        const char *id = modeset_at(&modes, i);
        kd_mode_t *m = create_mode(id, &cfg);
        if (m)
            shell_register_content_mode(m);
        else
            fprintf(stderr, "kdeskdash: no constructor for mode \"%s\" — "
                            "skipped\n", id);
    }
    shell_register_menu(menu_mode_create("menu", "Menu", &modes));

    /* Optional Redis: remote control + last-mode persistence. Safe when absent.
     * Register the persistence hook before starting so the restored/initial
     * mode is written back, then restore the last active mode if one exists. */
    redis_init(cfg.redis_host, cfg.redis_port, cfg.redis_auth);
    shell_set_change_cb(redis_set_active_mode);
    char last_mode[64];
    const char *restore =
        redis_get_active_mode(last_mode, sizeof(last_mode)) ? last_mode : NULL;
    shell_start(restore);

    /* Feeds are initialised only for modes this panel actually registered. The
     * handles were already isolated enough that a stray connection was harmless;
     * this makes it not happen at all, so a device without Dev never dials the
     * telemetry endpoint and never backs off against it. */

    /* Telemetry source (kpidash host metrics). Lazy connect on its own handle:
     * a down/slow endpoint never stalls boot or the control path. */
    if (modeset_enabled(&modes, "dev"))
        telemetry_init(cfg.telemetry_redis_host, cfg.telemetry_redis_port,
                       cfg.telemetry_redis_auth);

    /* Claude feed (agent activity + usage limits): third independent handle,
     * localhost instance on this Pi by default. */
    if (modeset_enabled(&modes, "claude"))
        claude_redis_init(cfg.claude_redis_host, cfg.claude_redis_port,
                          cfg.claude_redis_auth);

    /* kvscf window feed (foreground mode): its own endpoint, defaulting to the
     * claude-feed values because on rpidash2 both live on the same 6380
     * instance — but a second panel reads that same fleet claude feed while
     * driving a different kvscf. Own handle either way, for failure isolation.
     * The focus token comes from KVSCF_TOKEN (empty -> focusing disabled). */
    if (modeset_enabled(&modes, "foreground"))
        kvscf_redis_init(cfg.kvscf_redis_host, cfg.kvscf_redis_port,
                         cfg.kvscf_redis_auth, cfg.kvscf_token);

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
