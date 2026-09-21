/**
 * @file panel_cmd.h
 * The pure half of taking commands from central: who this panel answers to,
 * and what it will accept from the wire. No Redis, no LVGL, host-tested.
 *
 * The glue that actually reads `kdash:panelmode:{host}` and
 * `kdash:panelshot:{host}` is panel_feed.c; everything here is policy, which
 * is exactly the part worth pinning with tests.
 */
#ifndef KDESKDASH_PANEL_CMD_H
#define KDESKDASH_PANEL_CMD_H

#include <stdbool.h>
#include <stddef.h>

/* Room for a kdashdata host token (63 chars + NUL). */
#define PANEL_CMD_HOST_MAX 64

/* The one directory this panel will write a screenshot into. Trailing slash is
 * load-bearing: without it `/var/lib/kdeskdash2/x` would pass the prefix test. */
#define PANEL_CMD_SHOT_DIR "/var/lib/kdeskdash/"

/* Longest screenshot path accepted, matching the contract's own maxLength
 * (KDASH_PATH_MAX) so the two cannot disagree about what fits. */
#define PANEL_CMD_PATH_MAX 256

/* The `{host}` segment this panel answers to on central: the first label of
 * `raw`, lowercased, validated against the kdashdata token contract
 * ([A-Za-z0-9._-], 1..63).
 *
 * Lowercasing is not cosmetic. gethostname() on rpidash2 returns `rpiDash2`
 * — which is why that board's kpidash service-card key is mixed-case (korg
 * WI 2277) — while the fleet inventory, k-homelab and every other tool call
 * it `rpidash2`. A panel that published its commands under the kernel's
 * spelling would answer to a key nobody writes.
 *
 * False when `raw` is empty or its first label is not a legal token; `out`
 * is then set to "" and the caller must fall back (config, or no feed).
 */
bool panel_cmd_host(const char *raw, char *out, size_t outsz);

/* May the panel write a screenshot to `path`?
 *
 * `kdash:panelshot:{host}` is writable by every holder of the central Redis
 * password (CD-23), and the contract deliberately leaves the directory policy
 * to the panel, because only the panel knows which directories it owns. This
 * is that policy: inside PANEL_CMD_SHOT_DIR, non-empty remainder, no `..`
 * segment, no trailing slash, within PANEL_CMD_PATH_MAX.
 *
 * An absent path is not this function's business — that means "the panel's own
 * default", which is the ordinary case and never reaches here.
 */
bool panel_cmd_shot_path_ok(const char *path);

#endif /* KDESKDASH_PANEL_CMD_H */
