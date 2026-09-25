/**
 * @file lv_conf.h
 * Configuration file for LVGL v9.2.2 — kdeskdash (multi-mode desk dashboard)
 */

#if 1 /* Enable */

#ifndef LV_CONF_H
#define LV_CONF_H

/* Color depth: 32 = XRGB8888, matching the DRM framebuffer */
#define LV_COLOR_DEPTH 32

/* Use the standard C library */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

/* HAL */
#define LV_DEF_REFR_PERIOD  33      /* ~30 fps */
#define LV_DPI_DEF 130              /* reasonable for an 11.26" 1920x440 panel */

/* OS: pthreads (for mutex support in LVGL internals) */
#define LV_USE_OS   LV_OS_PTHREAD

/* Logging — enable for pre-MVP bring-up debugging.
 *
 * LV_LOG_PRINTF is 0 deliberately (WI #2657): lv_log_add() runs its printf
 * path AND any registered print callback, so leaving it on would print every
 * line twice. main.c registers the one sink with lv_log_register_print_cb(),
 * and src/logfilter.c decides what reaches it — LVGL warns about a missing
 * glyph on every redraw of every character the font lacks. */
#define LV_USE_LOG 1
#if LV_USE_LOG
    #define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
    #define LV_LOG_PRINTF 0
#endif

/* Fonts. LVGL's built-in Montserrat is OFF: those carry ASCII plus ° and •,
 * so an agent's em dash drew as a box (WI #2657). fonts/generate.sh builds the
 * same font with LVGL's own recipe and a wider range — Latin-1, dashes, curly
 * quotes, ellipsis — plus the LV_SYMBOL_* set, and the generated .c files are
 * committed. `just check-fonts` asserts they still carry what it declares. */
#define LV_FONT_CUSTOM_DECLARE \
    LV_FONT_DECLARE(kd_font_montserrat_14) \
    LV_FONT_DECLARE(kd_font_montserrat_20) \
    LV_FONT_DECLARE(kd_font_montserrat_28) \
    LV_FONT_DECLARE(kd_font_montserrat_36) \
    LV_FONT_DECLARE(kd_font_montserrat_48)
#define LV_FONT_DEFAULT &kd_font_montserrat_20

/* Layouts. Both default on, but the Launcher's button grid is the first place
 * that depends on GRID, so say so rather than inherit it. */
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/* Widgets */
#define LV_USE_BUTTON 1
#define LV_USE_LABEL  1
#define LV_USE_IMAGE  1
#define LV_USE_CANVAS 1

/* Drivers — Linux DRM for direct KMS rendering, evdev for capacitive touch */
#define LV_USE_LINUX_DRM 1
#define LV_USE_EVDEV     1

/* Others — snapshot renders the active screen to a memory buffer, used by the
 * kdeskdash:screenshot control-Redis trigger (device self-screenshot, e.g. for
 * README shots without photographing the glossy panel). */
#define LV_USE_SNAPSHOT 1

/* Runtime TTF rasterizer (stb_truetype) — the `icons` mode loads the vendored
 * Symbols Nerd Font at runtime and renders any of its ~10k glyphs at any size,
 * with no static font bake. The mode reads the deployed .ttf into memory itself
 * (plain POSIX) and uses lv_tiny_ttf_create_data, so LVGL's lv_fs file layer
 * (drive letters) is not needed — FILE_SUPPORT stays off. */
#define LV_USE_TINY_TTF 1
#if LV_USE_TINY_TTF
    #define LV_TINY_TTF_FILE_SUPPORT 0
    #define LV_TINY_TTF_CACHE_GLYPH_CNT 256
#endif

#endif /* LV_CONF_H */
#endif /* Enable */
