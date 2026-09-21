/**
 * @file test_panel_cmd.c
 * Host-only unit tests for the panel's central-command policy (no Redis/LVGL).
 *
 * Two policies, both of which fail silently in production if they are wrong:
 * a host segment nobody writes to means the panel simply never gets a command,
 * and a path guard that does not hold means a key any holder of the central
 * password can write decides where this root process puts a 2.5 MB file.
 */
#include <stdio.h>
#include <string.h>

#include "panel_cmd.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void host_is(const char *raw, const char *want) {
    char out[PANEL_CMD_HOST_MAX];
    bool ok = panel_cmd_host(raw, out, sizeof(out));
    if (!ok || strcmp(out, want) != 0) {
        fprintf(stderr, "FAIL: host(\"%s\"): got %s\"%s\", want \"%s\"\n", raw,
                ok ? "" : "(false) ", out, want);
        failures++;
    }
}

static void host_rejects(const char *raw, const char *why) {
    char out[PANEL_CMD_HOST_MAX];
    if (panel_cmd_host(raw, out, sizeof(out))) {
        fprintf(stderr, "FAIL: host(\"%s\") accepted (%s)\n", raw, why);
        failures++;
    } else if (out[0] != '\0') {
        fprintf(stderr, "FAIL: host(\"%s\") rejected but left \"%s\"\n", raw, out);
        failures++;
    }
}

static void path_ok(const char *p) {
    if (!panel_cmd_shot_path_ok(p)) {
        fprintf(stderr, "FAIL: path \"%s\" refused, should be allowed\n", p);
        failures++;
    }
}

static void path_refused(const char *p, const char *why) {
    if (panel_cmd_shot_path_ok(p)) {
        fprintf(stderr, "FAIL: path \"%s\" allowed (%s)\n", p, why);
        failures++;
    }
}

int main(void) {
    /* ---- the host segment ---- */
    {
        /* The case this exists for: the kernel's spelling on rpidash2 is
         * `rpiDash2` (korg WI 2277) and the fleet's is `rpidash2`. */
        host_is("rpiDash2", "rpidash2");
        host_is("rpiDash2.local", "rpidash2");
        host_is("rpidash3", "rpidash3");
        host_is("RPIDASH3.lan.example.com", "rpidash3");
        host_is("kai", "kai");
        /* Digits, dash and underscore all survive; only case changes. */
        host_is("Panel-01_A", "panel-01_a");

        host_rejects("", "empty hostname");
        host_rejects(".local", "empty first label");
        host_rejects("has space", "space is outside the token charset");
        host_rejects("has:colon", "a colon would forge a key segment");
        host_rejects("has/slash", "slash is outside the token charset");
        host_rejects(NULL, "NULL hostname");
        {
            /* 64 chars in the first label: one past the token contract. */
            char big[80];
            memset(big, 'a', 64);
            big[64] = '\0';
            host_rejects(big, "64-char label exceeds the 63-char token limit");
            big[63] = '\0';
            host_is(big, big); /* 63 is the boundary and is legal */
        }
        {
            /* An `out` too small to hold the label must fail, not truncate:
             * a truncated host is a different panel's key. */
            char small[4];
            check(!panel_cmd_host("rpidash2", small, sizeof(small)),
                  "a too-small out buffer fails rather than truncating");
        }
    }

    /* ---- the screenshot path guard ---- */
    {
        path_ok("/var/lib/kdeskdash/kdeskdash-shot.bmp");
        path_ok("/var/lib/kdeskdash/shots/deploy-0.28.0.bmp");
        path_ok("/var/lib/kdeskdash/a");

        /* The negative case the contract asks for by name: a path outside the
         * directory the panel owns. */
        path_refused("/tmp/anywhere.bmp", "outside the state directory");
        path_refused("/etc/cron.d/kdeskdash", "outside, and somewhere that runs");
        path_refused("/var/lib/kdeskdash", "the directory itself, not a file in it");
        path_refused("/var/lib/kdeskdash/", "the directory with a trailing slash");
        path_refused("/var/lib/kdeskdash2/x.bmp",
                     "a sibling directory sharing the prefix");
        path_refused("/var/lib/kdeskdashx", "prefix without the separator");

        /* Traversal, which is the way a conforming absolute path escapes. */
        path_refused("/var/lib/kdeskdash/../../../etc/shadow", "..  traversal");
        path_refused("/var/lib/kdeskdash/sub/../../etc/passwd", "nested .. traversal");
        path_refused("/var/lib/kdeskdash/..", "bare .. as the whole remainder");
        /* A file whose NAME merely starts with dots is not traversal. */
        path_ok("/var/lib/kdeskdash/..shot.bmp");
        path_ok("/var/lib/kdeskdash/...");

        path_refused(NULL, "NULL path");
        path_refused("", "empty path");
        path_refused("var/lib/kdeskdash/x.bmp", "relative path");
        {
            char big[PANEL_CMD_PATH_MAX + 32];
            int n = snprintf(big, sizeof(big), "%s", "/var/lib/kdeskdash/");
            memset(big + n, 'a', PANEL_CMD_PATH_MAX);
            big[n + PANEL_CMD_PATH_MAX] = '\0';
            path_refused(big, "longer than the contract's maxLength");
        }
    }

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_panel_cmd: all passed\n");
    return 0;
}
