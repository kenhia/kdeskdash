/**
 * @file chime.h
 * Audible alert when a Claude session goes BLOCKED — the pure core (WI 1096).
 *
 * No LVGL, no Redis, no ALSA: this owns the decisions worth testing, and
 * chime_watch.c is the glue that feeds it a session snapshot and plays the
 * sound it asks for.
 *
 *   - EDGE, not level. A session chimes on the transition INTO
 *     KDASH_CLAUDE_DISP_BLOCKED, once, never again while it stays blocked. It
 *     chimes again only after leaving BLOCKED (answered, or aged to STALE, or
 *     ended) and re-entering it.
 *   - The first complete snapshot PRIMES and never chimes: a restart or a
 *     deploy must not sound for a session that was already blocked before the
 *     panel came up.
 *   - A failed read is not a snapshot. The caller does not call observe() for
 *     one, so a feed blip cannot clear the set and re-chime every blocked
 *     session on recovery.
 *   - A cooldown so several sessions blocking at once sound once, not a burst.
 *     An edge swallowed by the cooldown is consumed, not deferred.
 *
 * Only BLOCKED. AWAITING fires on every Stop hook fleet-wide and would chime
 * every few seconds, which destroys the signal (WI 1096's agreed scope).
 */
#ifndef KDESKDASH_CHIME_H
#define KDESKDASH_CHIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kdash/kdash_payload.h"

/* Tracked blocked sessions. Matches the claude mode's store cap; a session past
 * it is simply not tracked, which errs toward silence, never toward a burst. */
#define CHIME_TRACK_MAX 24

/* "host\x1fsid" — both halves are KDASH_TOKEN_MAX tokens. */
#define CHIME_ID_MAX (2 * KDASH_TOKEN_MAX)

typedef struct {
    bool primed;
    long long cooldown_s;
    long long last_chime; /* 0 = never */
    int nblocked;
    char blocked[CHIME_TRACK_MAX][CHIME_ID_MAX];
} chime_t;

void chime_init(chime_t *c, long long cooldown_s);

/* Feed one COMPLETE snapshot whose `disp` has been derived for `now`
 * (kdash_claude_sessions_refresh). Returns true when the caller should play
 * the chime now. Updates the tracked set either way. */
bool chime_observe(chime_t *c, const kdash_claude_session_t *s, int n,
                   long long now);

/* ---- the built-in sound ----
 *
 * Two soft sine notes (E6 then B6, a rising fifth), each with a fast attack
 * and an exponential decay — a doorbell "ding-ding", well under a second.
 * Rendered at the USB speaker's native format (S16_LE, 48 kHz, stereo) so the
 * ALSA plug layer has nothing to convert. */
#define CHIME_RATE     48000
#define CHIME_CHANNELS 2
#define CHIME_FRAMES   (CHIME_RATE * 6 / 10) /* 0.6 s */

/* Fill `out` (frames * CHIME_CHANNELS interleaved samples) with the chime at
 * `volume` percent of full scale (clamped to 0..100). Returns frames written,
 * which is min(frames, CHIME_FRAMES). */
size_t chime_tone_render(int16_t *out, size_t frames, int volume);

/* ---- the device ----
 *
 * The ALSA card NAME inside a device string — "UACDemoV10" from
 * "plughw:CARD=UACDemoV10,DEV=0" — which is what /proc/asound/<name> is
 * called, so the glue can tell "speaker unplugged" from "configured". False
 * when the string names no CARD= (e.g. "default", "hw:3"); the caller then
 * cannot check presence and just tries. */
bool chime_card_name(const char *device, char *out, size_t outsz);

#endif /* KDESKDASH_CHIME_H */
