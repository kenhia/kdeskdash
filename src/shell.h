/**
 * @file shell.h
 * The mode shell: owns the set of modes, the active mode, gesture-driven
 * navigation, and the per-loop lifecycle tick.
 *
 * Navigation model:
 *   - swipe left/right  -> next/previous content mode (wrapping)
 *   - swipe down        -> menu
 *   - tapping a menu tile (handled by the menu mode) -> open that mode
 *
 * The shell does not own the mode_t storage; callers keep registered modes
 * alive for the lifetime of the program.
 */
#ifndef KDESKDASH_SHELL_H
#define KDESKDASH_SHELL_H

#include "mode.h"

/* Reset shell state. Call once after lv_init() and display creation. */
void shell_init(void);

/* Register a content mode (appears in the swipe cycle and the menu). */
void shell_register_content_mode(kd_mode_t *m);

/* Register the menu mode (the swipe-down target and startup default). */
void shell_register_menu(kd_mode_t *m);

/* Switch to a specific mode: deactivate the old, activate the new, show it. */
void shell_set_active(kd_mode_t *m);

/* Look up a registered mode (content or menu) by id; NULL if unknown. */
kd_mode_t *shell_find_mode(const char *id);

/* The currently active mode, or NULL before shell_start(). */
kd_mode_t *shell_active(void);

/* The next/previous content mode relative to the active one (wrapping).
 * If the active mode is not a content mode (e.g. the menu), returns the
 * first content mode. Returns the active mode if there are no content modes. */
kd_mode_t *shell_next_content(void);
kd_mode_t *shell_prev_content(void);

/* Enumerate registered content modes (for the menu launcher). */
int        shell_content_count(void);
kd_mode_t *shell_content_at(int index);

/* Drive the active mode's tick. Call once per main-loop iteration. */
void shell_tick(void);

/* Register a callback invoked with the new mode id whenever the active mode
 * changes. Used to persist the active mode (e.g. to Redis). Pass NULL to
 * clear. The callback fires after the new mode is shown. */
void shell_set_change_cb(void (*cb)(const char *id));

/* Start the shell. If `restore_id` names a registered mode, open it; otherwise
 * open the menu, or the first content mode if no menu is registered. */
void shell_start(const char *restore_id);

#endif /* KDESKDASH_SHELL_H */
