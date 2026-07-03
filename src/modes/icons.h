/**
 * @file icons.h
 * Nerd Font icon-browser mode: page a glyph set in a touch grid, preview the
 * selected glyph at several sizes, and curate a favourites list saved to disk.
 * Renders via LVGL's runtime TinyTTF engine over the vendored Symbols Nerd Font.
 */
#ifndef KDESKDASH_MODE_ICONS_H
#define KDESKDASH_MODE_ICONS_H

#include "mode.h"

/* Create the icons mode. `ttf_path` is the Symbols Nerd Font read at runtime;
 * `fav_path` is where the favourites file is loaded from / saved to. Both are
 * borrowed for the mode's lifetime (config env strings, process-stable). */
kd_mode_t *icons_mode_create(const char *id, const char *title,
                             const char *ttf_path, const char *fav_path);

#endif /* KDESKDASH_MODE_ICONS_H */
