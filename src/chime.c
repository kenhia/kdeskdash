/**
 * @file chime.c
 * Audible alert when a Claude session goes BLOCKED — the pure core. See chime.h.
 */
#include "chime.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void chime_init(chime_t *c, long long cooldown_s) {
    memset(c, 0, sizeof(*c));
    c->cooldown_s = cooldown_s < 0 ? 0 : cooldown_s;
}

static void session_id(const kdash_claude_session_t *s, char *out) {
    snprintf(out, CHIME_ID_MAX, "%s\x1f%s", s->host, s->sid);
}

static bool was_blocked(const chime_t *c, const char *id) {
    for (int i = 0; i < c->nblocked; i++)
        if (strcmp(c->blocked[i], id) == 0)
            return true;
    return false;
}

bool chime_observe(chime_t *c, const kdash_claude_session_t *s, int n,
                   long long now) {
    char next[CHIME_TRACK_MAX][CHIME_ID_MAX];
    int nnext = 0;
    bool edge = false;

    for (int i = 0; i < n && nnext < CHIME_TRACK_MAX; i++) {
        if (s[i].disp != KDASH_CLAUDE_DISP_BLOCKED)
            continue;
        session_id(&s[i], next[nnext]);
        if (!was_blocked(c, next[nnext]))
            edge = true;
        nnext++;
    }

    memcpy(c->blocked, next, sizeof(next[0]) * (size_t)nnext);
    c->nblocked = nnext;

    if (!c->primed) {
        c->primed = true; /* already blocked at startup: not news */
        return false;
    }
    if (!edge)
        return false;
    if (c->last_chime != 0 && now - c->last_chime < c->cooldown_s)
        return false; /* consumed, not deferred */
    c->last_chime = now;
    return true;
}

/* One note: a 5 ms linear attack (no click) then exponential decay. */
static double note(double t, double start, double freq, double tau) {
    double u = t - start;
    if (u < 0)
        return 0;
    double env = (u < 0.005 ? u / 0.005 : 1.0) * exp(-u / tau);
    return env * sin(2.0 * M_PI * freq * u);
}

size_t chime_tone_render(int16_t *out, size_t frames, int volume) {
    if (volume < 0)
        volume = 0;
    if (volume > 100)
        volume = 100;
    if (frames > CHIME_FRAMES)
        frames = CHIME_FRAMES;

    double amp = 32767.0 * volume / 100.0;
    for (size_t i = 0; i < frames; i++) {
        double t = (double)i / CHIME_RATE;
        /* Each note peaks at 0.5 so their overlap can never exceed 1.0. */
        double v = 0.5 * note(t, 0.0, 1318.51, 0.09) +  /* E6 */
                   0.5 * note(t, 0.16, 1975.53, 0.12); /* B6 */
        /* Fade the last 20 ms to exactly zero so the stream never ends on a step. */
        double tail = (double)(CHIME_FRAMES - 1 - i) / (CHIME_RATE * 0.02);
        if (tail < 1.0)
            v *= tail;
        int16_t sample = (int16_t)lrint(v * amp);
        for (int ch = 0; ch < CHIME_CHANNELS; ch++)
            out[i * CHIME_CHANNELS + ch] = sample;
    }
    return frames;
}

bool chime_card_name(const char *device, char *out, size_t outsz) {
    if (!device || !out || outsz == 0)
        return false;
    const char *p = strstr(device, "CARD=");
    if (!p)
        return false;
    p += 5;
    size_t len = strcspn(p, ",");
    if (len == 0 || len >= outsz)
        return false;
    /* /proc/asound/<name>: refuse anything that could walk out of it. */
    if (memchr(p, '/', len) || strncmp(p, ".", len) == 0 ||
        strncmp(p, "..", len) == 0)
        return false;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}
