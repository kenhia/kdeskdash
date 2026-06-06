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

/* Logging — enable for pre-MVP bring-up debugging */
#define LV_USE_LOG 1
#if LV_USE_LOG
    #define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
    #define LV_LOG_PRINTF 1
#endif

/* Fonts (built-in Montserrat — no font-conversion pipeline needed for the pre-MVP) */
#define LV_FONT_MONTSERRAT_14  1
#define LV_FONT_MONTSERRAT_20  1
#define LV_FONT_MONTSERRAT_28  1
#define LV_FONT_MONTSERRAT_36  1
#define LV_FONT_DEFAULT &lv_font_montserrat_20

/* Widgets */
#define LV_USE_BUTTON 1
#define LV_USE_LABEL  1
#define LV_USE_IMAGE  1
#define LV_USE_CANVAS 1

/* Drivers — Linux DRM for direct KMS rendering, evdev for capacitive touch */
#define LV_USE_LINUX_DRM 1
#define LV_USE_EVDEV     1

#endif /* LV_CONF_H */
#endif /* Enable */
