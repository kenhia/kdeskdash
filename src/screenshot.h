/**
 * @file screenshot.h
 * Device self-screenshot: render the active LVGL screen to a memory buffer
 * (lv_snapshot) and write it as a 24-bit BMP, so a pixel-perfect shot can be
 * taken without photographing the glossy panel.
 *
 * Triggered from central by `kdash:panelshot:<host>` (see panel_feed.c). The
 * write is ATOMIC — bmp_write_file_atomic does temp + rename — so a consumer
 * polling for the file never reads a partial one (korg WI 2308).
 *
 * A path arriving on the wire has already passed panel_cmd_shot_path_ok()
 * before it gets here; this function trusts its caller.
 */
#ifndef KDESKDASH_SCREENSHOT_H
#define KDESKDASH_SCREENSHOT_H

#include <stdbool.h>

/* Default output path when the trigger key carries no path of its own.
 *
 * The state directory, not /tmp: the systemd unit sets PrivateTmp=yes, so a
 * shot written to /tmp lands in the service's private namespace where scripts
 * on the device (scripts/kddss) cannot see it. StateDirectory=kdeskdash is the
 * one path that is both writable under ProtectSystem=strict and visible on the
 * host filesystem. */
#define SCREENSHOT_DEFAULT_PATH "/var/lib/kdeskdash/kdeskdash-shot.bmp"

/* Snapshot the active screen to `path` (BMP). Runs on the UI thread; the
 * render takes a few tens of ms at 1920x440 — fine for a one-shot trigger.
 * Returns false (and logs to stderr) on snapshot or file failure. */
bool screenshot_save(const char *path);

#endif /* KDESKDASH_SCREENSHOT_H */
