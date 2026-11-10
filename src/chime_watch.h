/**
 * @file chime_watch.h
 * Main-loop watcher that chimes when a Claude session goes BLOCKED (WI 1096).
 *
 * Not a mode, on purpose: the alert has to sound whatever is on screen, and a
 * mode does no work while deactivated. So this runs from main.c's loop on its
 * OWN libkdash handle on KDASH_STEM_CLAUDE — not the claude mode's, which
 * exists only when that mode is registered (rpidash3, the board with the
 * speaker, does not register it).
 *
 * Off unless KDESKDASH_CHIME_DEVICE names an ALSA device. When it is off the
 * handle is never created and the feed is never dialled. When the named card
 * is not present the panel stays silent and says so once; it never fails to
 * start over a speaker.
 */
#ifndef KDESKDASH_CHIME_WATCH_H
#define KDESKDASH_CHIME_WATCH_H

/* Defaults, here rather than in chime.h so config.c needs no libkdash header. */
#define CHIME_DEFAULT_COOLDOWN_S 30
#define CHIME_DEFAULT_VOLUME     30 /* percent of full scale, built-in tone */

typedef struct {
    const char *device;     /* ALSA device; NULL/"" = chime off */
    const char *sound_path; /* WAV to play instead of the built-in tone; NULL = built-in */
    int volume;             /* 0..100, built-in tone only */
    long long cooldown_s;   /* minimum gap between chimes */
    const char *redis_host; /* the claude feed's endpoint */
    int redis_port;
    const char *redis_auth; /* NULL = no AUTH */
} chime_watch_opts_t;

void chime_watch_init(const chime_watch_opts_t *opts);

/* Call from the main loop (~1 Hz is plenty); throttles its own SCAN and reaps
 * the player. Never blocks on audio. No-op when off. */
void chime_watch_tick(void);

void chime_watch_shutdown(void);

#endif /* KDESKDASH_CHIME_WATCH_H */
