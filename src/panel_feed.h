/**
 * @file panel_feed.h
 * Commands from central, read-only: `kdash:panelmode:<host>` (switch mode,
 * optionally injecting settings) and `kdash:panelshot:<host>` (take a
 * screenshot). The kdashdata control cluster, CD-22, read through libkdash's
 * typed readers on this panel's own handle.
 *
 * This replaces the three things the board's own Redis used to do: the
 * `kdeskdash:active_mode` poll, the `gol:settings` / `golz:settings`
 * HGETALL+DEL injections, and the `screenshot` GETDEL. The shape of the change
 * is the point:
 *
 *   - **The panel never clears a command.** It remembers the `ts` of the last
 *     command it acted on, per verb, and acts only when a newer one arrives
 *     (kdash_cmd_actionable, CD-17). A manual mode change on the panel
 *     therefore STICKS — the key still says "GoL", but its `ts` has not moved,
 *     so nothing yanks the screen back.
 *   - **Each verb keeps its own stamp.** A mode switch and a screenshot are
 *     separate edges; one stamp for both would make acting on either suppress
 *     the next of the other kind.
 *   - **Old commands are ignored at startup.** With no stamp yet, the window
 *     (KDASH_PANEL_WINDOW_S) is what stops a panel that was off for an hour
 *     replaying the switch it missed.
 *   - **Degrade, don't block (CD-6).** Central unreachable means no remote
 *     commands; touch and the state file keep working, and nothing stalls.
 *
 * The panel is a CONSUMER here — it never writes these keys.
 */
#ifndef KDESKDASH_PANEL_FEED_H
#define KDESKDASH_PANEL_FEED_H

#include <stdbool.h>

#include "gol.h"
#include "golz.h"

/* Open the handle on the central endpoint and record which `{host}` segment
 * this panel answers to. `panel_host` is the lowercase inventory name (see
 * panel_cmd_host) — an empty or invalid one disables the feed entirely rather
 * than guessing, because the wrong host segment means silently answering
 * another panel's commands.
 *
 * `auth` NULL or "" means no AUTH. Never connects here: the first poll does,
 * and its failure costs one backoff interval, not a slow boot.
 */
void panel_feed_init(const char *host, int port, const char *auth,
                     const char *panel_host);

/* One poll cycle — both verbs, one round trip each. Drive it from the main
 * loop at the same ~1 Hz the Redis control poll ran at. No-op when the feed is
 * disabled or the endpoint is down. */
void panel_feed_poll(void);

/* Close the handle (idempotent). */
void panel_feed_shutdown(void);

/* Consume any settings the last actionable mode command carried, overwriting
 * only the fields it named — absent fields keep the caller's randomized
 * defaults, exactly as HGETALL-and-apply-present-fields did. Returns true if
 * anything was applied.
 *
 * Two functions over ONE pending set, each with its own consume flag, because
 * GoLZ's reseed reads both vocabularies in sequence: if the Conway half
 * consumed the pending settings, the GoLZ half would find nothing. The old
 * feed got this for free by having two Redis keys; the contract carries one
 * `settings` object, so the flags are how the port stays behaviour-identical.
 */
bool panel_feed_apply_gol_settings(gol_settings_t *cfg);
bool panel_feed_apply_golz_settings(golz_settings_t *cfg);

#endif /* KDESKDASH_PANEL_FEED_H */
