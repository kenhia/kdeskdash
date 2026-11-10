/**
 * @file test_chime.c
 * Host-only unit tests for the BLOCKED chime core (no Redis, no ALSA).
 *
 * The failure modes worth pinning are all about WHEN it sounds: a chime that
 * repeats while a session stays blocked is an alarm, one that sounds on every
 * restart is noise, and one that re-sounds after a feed blip teaches Ken to
 * ignore it. Each has a case below.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chime.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static kdash_claude_session_t S(const char *host, const char *sid,
                                kdash_claude_disp_t d) {
    kdash_claude_session_t s;
    memset(&s, 0, sizeof(s));
    snprintf(s.host, sizeof(s.host), "%s", host);
    snprintf(s.sid, sizeof(s.sid), "%s", sid);
    s.disp = d;
    return s;
}

#define B KDASH_CLAUDE_DISP_BLOCKED
#define A KDASH_CLAUDE_DISP_AWAITING
#define W KDASH_CLAUDE_DISP_WORKING
#define X KDASH_CLAUDE_DISP_STALE

static void test_edge_not_level(void) {
    chime_t c;
    chime_init(&c, 0);
    kdash_claude_session_t s[1] = {S("kai", "a", W)};
    check(!chime_observe(&c, s, 1, 100), "priming snapshot never chimes");
    s[0].disp = B;
    check(chime_observe(&c, s, 1, 105), "working -> blocked chimes");
    check(!chime_observe(&c, s, 1, 110), "still blocked: no repeat");
    check(!chime_observe(&c, s, 1, 500), "still blocked much later: no repeat");
    s[0].disp = W;
    check(!chime_observe(&c, s, 1, 505), "leaving blocked is silent");
    s[0].disp = B;
    check(chime_observe(&c, s, 1, 510), "re-entering blocked chimes again");
}

static void test_awaiting_is_not_blocked(void) {
    chime_t c;
    chime_init(&c, 0);
    kdash_claude_session_t s[1] = {S("kai", "a", W)};
    chime_observe(&c, s, 1, 100);
    s[0].disp = A;
    check(!chime_observe(&c, s, 1, 105), "awaiting never chimes");
    s[0].disp = X;
    check(!chime_observe(&c, s, 1, 110), "stale never chimes");
}

static void test_blocked_at_startup_is_not_news(void) {
    chime_t c;
    chime_init(&c, 0);
    kdash_claude_session_t s[2] = {S("kai", "a", B), S("cleo", "b", W)};
    check(!chime_observe(&c, s, 2, 100), "blocked at boot: primed, silent");
    check(!chime_observe(&c, s, 2, 105), "and silent after");
    s[1].disp = B;
    check(chime_observe(&c, s, 2, 110), "a NEW block after priming chimes");
}

static void test_empty_snapshot_primes(void) {
    chime_t c;
    chime_init(&c, 0);
    check(!chime_observe(&c, NULL, 0, 100), "nobody running primes");
    kdash_claude_session_t s[1] = {S("kai", "a", B)};
    check(chime_observe(&c, s, 1, 105), "first block after an empty prime chimes");
}

static void test_session_identity_is_host_and_sid(void) {
    chime_t c;
    chime_init(&c, 0);
    kdash_claude_session_t s[1] = {S("kai", "a", B)};
    chime_observe(&c, s, 1, 100);
    /* Same sid on another host is another session. */
    kdash_claude_session_t t[2] = {S("kai", "a", B), S("cleo", "a", B)};
    check(chime_observe(&c, t, 2, 105), "same sid, new host: new session");
    /* Order of the snapshot does not matter. */
    kdash_claude_session_t u[2] = {S("cleo", "a", B), S("kai", "a", B)};
    check(!chime_observe(&c, u, 2, 110), "reordered snapshot: no edge");
}

static void test_ended_session_reappearing(void) {
    chime_t c;
    chime_init(&c, 0);
    kdash_claude_session_t s[1] = {S("kai", "a", B)};
    chime_observe(&c, NULL, 0, 100);
    check(chime_observe(&c, s, 1, 105), "blocks");
    check(!chime_observe(&c, NULL, 0, 110), "gone: silent");
    check(chime_observe(&c, s, 1, 115), "back and blocked: chimes");
}

static void test_cooldown(void) {
    chime_t c;
    chime_init(&c, 30);
    kdash_claude_session_t s[3] = {S("kai", "a", W), S("kai", "b", W),
                                   S("kai", "c", W)};
    chime_observe(&c, s, 3, 100);
    s[0].disp = B;
    s[1].disp = B;
    check(chime_observe(&c, s, 3, 105), "two at once: one chime");
    s[2].disp = B;
    check(!chime_observe(&c, s, 3, 120), "third inside cooldown: silent");
    check(!chime_observe(&c, s, 3, 140), "swallowed edge is consumed, not deferred");
    s[0].disp = W;
    chime_observe(&c, s, 3, 141);
    s[0].disp = B;
    check(chime_observe(&c, s, 3, 142), "new edge after cooldown chimes");
}

static void test_failed_read_is_not_a_snapshot(void) {
    /* The contract is the CALLER's: a -1 read is never passed to observe().
     * What the core guarantees is that skipping calls loses nothing — the set
     * survives untouched, so recovery re-reads the same blocked sessions and
     * finds no edge. */
    chime_t c;
    chime_init(&c, 0);
    kdash_claude_session_t s[1] = {S("kai", "a", W)};
    chime_observe(&c, s, 1, 100);
    s[0].disp = B;
    check(chime_observe(&c, s, 1, 105), "blocks");
    /* ... several failed reads: no calls ... */
    check(!chime_observe(&c, s, 1, 200), "recovery after a blip: silent");
}

static void test_tracking_cap_errs_silent(void) {
    chime_t c;
    chime_init(&c, 0);
    kdash_claude_session_t s[CHIME_TRACK_MAX + 2];
    for (int i = 0; i < CHIME_TRACK_MAX + 2; i++) {
        char sid[8];
        snprintf(sid, sizeof(sid), "s%d", i);
        s[i] = S("kai", sid, B);
    }
    chime_observe(&c, s, CHIME_TRACK_MAX + 2, 100);
    check(c.nblocked == CHIME_TRACK_MAX, "tracked set capped");
    check(!chime_observe(&c, s, CHIME_TRACK_MAX + 2, 105),
          "same overfull snapshot: no edge");
}

static void test_tone(void) {
    static int16_t buf[CHIME_FRAMES * CHIME_CHANNELS + 64];
    size_t n = chime_tone_render(buf, CHIME_FRAMES + 32, 30);
    check(n == CHIME_FRAMES, "render clamps to CHIME_FRAMES");

    int peak = 0;
    for (size_t i = 0; i < n * CHIME_CHANNELS; i++)
        if (abs(buf[i]) > peak)
            peak = abs(buf[i]);
    check(peak > 32767 * 30 / 100 / 4, "audible at 30%");
    check(peak <= 32767 * 30 / 100 + 1, "never exceeds the volume ceiling");

    check(buf[0] == 0, "starts at zero (no click)");
    check(buf[(n - 1) * CHIME_CHANNELS] == 0, "ends at zero (no click)");
    int stereo = 1;
    for (size_t i = 0; i < n; i++)
        if (buf[i * 2] != buf[i * 2 + 1])
            stereo = 0;
    check(stereo, "both channels identical");

    chime_tone_render(buf, CHIME_FRAMES, 0);
    int silent = 1;
    for (size_t i = 0; i < CHIME_FRAMES * CHIME_CHANNELS; i++)
        if (buf[i] != 0)
            silent = 0;
    check(silent, "volume 0 is silence");

    chime_tone_render(buf, CHIME_FRAMES, 250);
    peak = 0;
    for (size_t i = 0; i < CHIME_FRAMES * CHIME_CHANNELS; i++)
        if (abs(buf[i]) > peak)
            peak = abs(buf[i]);
    check(peak <= 32767, "volume clamps at 100 (no wrap)");
    check(peak > 32767 / 4, "volume 100 is loud");
}

static void test_card_name(void) {
    char out[64];
    check(chime_card_name("plughw:CARD=UACDemoV10,DEV=0", out, sizeof(out)) &&
              strcmp(out, "UACDemoV10") == 0,
          "plughw CARD= name");
    check(chime_card_name("default:CARD=UACDemoV10", out, sizeof(out)) &&
              strcmp(out, "UACDemoV10") == 0,
          "trailing CARD= name");
    check(!chime_card_name("default", out, sizeof(out)), "no CARD=");
    check(!chime_card_name("hw:3", out, sizeof(out)), "index form");
    check(!chime_card_name("plughw:CARD=,DEV=0", out, sizeof(out)), "empty");
    check(!chime_card_name("plughw:CARD=../x,DEV=0", out, sizeof(out)), "slash");
    check(!chime_card_name("plughw:CARD=..,DEV=0", out, sizeof(out)), "dotdot");
    check(!chime_card_name("plughw:CARD=UACDemoV10", out, 4), "too long");
    check(!chime_card_name(NULL, out, sizeof(out)), "NULL");
}

int main(void) {
    test_edge_not_level();
    test_awaiting_is_not_blocked();
    test_blocked_at_startup_is_not_news();
    test_empty_snapshot_primes();
    test_session_identity_is_host_and_sid();
    test_ended_session_reappearing();
    test_cooldown();
    test_failed_read_is_not_a_snapshot();
    test_tracking_cap_errs_silent();
    test_tone();
    test_card_name();
    if (failures) {
        fprintf(stderr, "test_chime: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_chime: all passed\n");
    return 0;
}
