/**
 * @file chime_watch.c
 * BLOCKED-session chime: the feed + player glue. See chime_watch.h.
 *
 * The decisions (edge, prime, cooldown) and the sound itself are the pure
 * chime core's and are host-tested; this file reads the feed and runs aplay.
 *
 * The player is a fork/exec of `aplay`, fire-and-forget and reaped with
 * WNOHANG, so the LVGL loop never waits on audio. alsa-utils is already on
 * both boards and there is no PulseAudio/PipeWire — bare ALSA — so this adds
 * no build dependency (no libasound in the cross sysroot). The built-in tone
 * is piped as raw PCM through a short-lived writer child, so nothing is ever
 * written to disk.
 */
#include "chime_watch.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "chime.h"
#include "kdash/kdash_conn.h"
#include "kdash/kdash_endpoint.h"
#include "kdash/kdash_feed.h"
#include "kdash/kdash_payload.h"

/* The claude mode's SCAN cadence (claude.c CLAUDE_SCAN_MS), for the same
 * reason: this Redis is shared by three dashboards. A block is noticed within
 * 5 s, which is well inside "noticed without watching the screen". */
#define CHIME_SCAN_S 5

#define SESSIONS_MAX 24

static bool          g_on;
static kdash_conn_t *g_conn;
static chime_t       g_chime;
static char          g_device[128];
static char          g_card[64]; /* "" = device names no CARD=, cannot probe */
static char          g_sound[256];
static time_t        g_last_scan;
static pid_t         g_player = -1;
static bool          g_absent_logged;
static bool          g_feed_down_logged;

static int16_t g_tone[CHIME_FRAMES * CHIME_CHANNELS];
static size_t  g_tone_frames;

/* Present = /proc/asound/<card> exists. A device string with no CARD= cannot
 * be probed, so it is assumed present and aplay is left to report. Logs the
 * transition, once each way, so an unplugged speaker is one journal line and
 * not one per block. */
static bool device_present(void) {
    if (g_card[0] == '\0')
        return true;
    char path[96];
    snprintf(path, sizeof(path), "/proc/asound/%s", g_card);
    struct stat st;
    bool present = stat(path, &st) == 0;
    if (!present && !g_absent_logged) {
        printf("kdeskdash: chime: ALSA card %s not present — staying silent "
               "until it appears\n", g_card);
        g_absent_logged = true;
    } else if (present && g_absent_logged) {
        printf("kdeskdash: chime: ALSA card %s present\n", g_card);
        g_absent_logged = false;
    }
    return present;
}

/* In a child: close everything but stdio, so aplay does not inherit the DRM
 * and evdev descriptors (or a Redis socket) for the life of the sound. */
static void close_inherited(void) {
    long max = sysconf(_SC_OPEN_MAX);
    if (max < 0 || max > 4096)
        max = 4096;
    for (int fd = 3; fd < max; fd++)
        close(fd);
}

static void exec_aplay_raw(void) {
    execlp("aplay", "aplay", "-q", "-D", g_device, "-t", "raw", "-f", "S16_LE",
           "-r", "48000", "-c", "2", "-", (char *)NULL);
    _exit(127);
}

/* The writer child: start aplay on a pipe and feed it the tone. Exits with
 * aplay's status so the parent's reap sees the real outcome. */
static void run_tone_writer(void) {
    close_inherited();
    int fds[2];
    if (pipe(fds) != 0)
        _exit(126);
    pid_t ap = fork();
    if (ap < 0)
        _exit(126);
    if (ap == 0) {
        dup2(fds[0], STDIN_FILENO);
        close_inherited();
        exec_aplay_raw();
    }
    close(fds[0]);
    const char *p = (const char *)g_tone;
    size_t left = g_tone_frames * CHIME_CHANNELS * sizeof(int16_t);
    while (left > 0) {
        ssize_t w = write(fds[1], p, left);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            break; /* aplay died; its status says why */
        p += w;
        left -= (size_t)w;
    }
    close(fds[1]);
    int status = 0;
    while (waitpid(ap, &status, 0) < 0 && errno == EINTR)
        ;
    _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 125);
}

static void play(void) {
    if (g_player > 0)
        return; /* one at a time; the cooldown makes this all but moot */
    if (!device_present())
        return;
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "kdeskdash: chime: fork failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN); /* a dead aplay is a write error, not a kill */
        if (g_sound[0] != '\0') {
            close_inherited();
            execlp("aplay", "aplay", "-q", "-D", g_device, g_sound, (char *)NULL);
            _exit(127);
        }
        run_tone_writer();
    }
    g_player = pid;
    printf("kdeskdash: chime: session BLOCKED — playing %s on %s\n",
           g_sound[0] ? g_sound : "built-in tone", g_device);
}

static void reap(void) {
    if (g_player <= 0)
        return;
    int status;
    pid_t r = waitpid(g_player, &status, WNOHANG);
    if (r == 0)
        return;
    g_player = -1;
    if (r < 0)
        return;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
        fprintf(stderr, "kdeskdash: chime: aplay not found (alsa-utils)\n");
    else
        fprintf(stderr, "kdeskdash: chime: aplay failed on %s (status %d)\n",
                g_device, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

void chime_watch_init(const chime_watch_opts_t *o) {
    g_on = false;
    if (!o || !o->device || o->device[0] == '\0') {
        printf("kdeskdash: chime: off (KDESKDASH_CHIME_DEVICE unset)\n");
        return;
    }
    snprintf(g_device, sizeof(g_device), "%s", o->device);
    snprintf(g_sound, sizeof(g_sound), "%s", o->sound_path ? o->sound_path : "");
    if (!chime_card_name(g_device, g_card, sizeof(g_card)))
        g_card[0] = '\0';
    g_tone_frames = chime_tone_render(g_tone, CHIME_FRAMES, o->volume);
    chime_init(&g_chime, o->cooldown_s);
    g_last_scan = 0;
    g_absent_logged = false;
    g_feed_down_logged = false;

    /* Same handle shape as the claude mode's (claude.c): its own stem, an
     * explicit endpoint so khlenv is never consulted, and "" rather than NULL
     * for "no AUTH" — libkdash reads $REDISCLI_AUTH on NULL. */
    kdash_conn_opts_t opts = {
        .app = "kdeskdash",
        .stem = &KDASH_STEM_CLAUDE,
        .host = o->redis_host,
        .port = o->redis_port,
        .auth = o->redis_auth ? o->redis_auth : "",
    };
    g_conn = kdash_conn_new(&opts);
    if (!g_conn)
        return;
    g_on = true;
    printf("kdeskdash: chime: on — device %s, %s, cooldown %llds, feed %s:%d\n",
           g_device,
           g_sound[0] ? g_sound : "built-in tone",
           (long long)o->cooldown_s, o->redis_host, o->redis_port);
    if (g_sound[0] == '\0')
        printf("kdeskdash: chime: built-in tone at volume %d%%\n", o->volume);
    device_present(); /* say now, not at the first block, if it is missing */
}

void chime_watch_tick(void) {
    if (!g_on)
        return;
    reap();

    time_t now = time(NULL);
    if (g_last_scan != 0 && now - g_last_scan < CHIME_SCAN_S)
        return;
    g_last_scan = now;

    kdash_claude_session_t s[SESSIONS_MAX];
    int n = kdash_claude_sessions(g_conn, s, SESSIONS_MAX, NULL);
    if (n < 0) {
        /* Counted-reader rule: not a snapshot. Skipping observe() keeps the
         * tracked set, so recovery does not re-chime every blocked session. */
        if (!g_feed_down_logged) {
            fprintf(stderr, "kdeskdash: chime: claude feed unavailable\n");
            g_feed_down_logged = true;
        }
        return;
    }
    if (g_feed_down_logged) {
        printf("kdeskdash: chime: claude feed back\n");
        g_feed_down_logged = false;
    }
    kdash_claude_sessions_refresh(s, n, (long long)now, KDASH_CLAUDE_IDLE_S,
                                  KDASH_CLAUDE_STALE_S);
    if (chime_observe(&g_chime, s, n, (long long)now))
        play();
}

void chime_watch_shutdown(void) {
    if (g_player > 0)
        waitpid(g_player, NULL, 0); /* under a second; never a zombie */
    g_player = -1;
    kdash_conn_free(g_conn);
    g_conn = NULL;
    g_on = false;
}
