/**
 * @file test_panel_state.c
 * Host-only unit tests for the durable panel state file (no Redis, no LVGL).
 *
 * Two things are worth pinning here and they pull in opposite directions:
 * the file is rejected WHOLE when it is malformed, and an UNKNOWN KEY is the
 * one thing that does not reject it — because naming an older version is this
 * project's rollback, and a key a later build added must not brick the state
 * file on the way back down.
 *
 * The atomic write is tested for what it promises: the temp file is gone
 * afterwards, and a failed write leaves the previous target byte-identical.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "panel_state.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void eq_str(const char *got, const char *want, const char *what) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL: %s: got \"%s\", want \"%s\"\n", what, got, want);
        failures++;
    }
}

static void eq_long(long got, long want, const char *what) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got %ld, want %ld\n", what, got, want);
        failures++;
    }
}

/* Parse a NUL-terminated literal. */
static bool parse(const char *text, panel_state_t *out) {
    return panel_state_parse(text, strlen(text), out);
}

/* A scratch path unique to this process, so a parallel ctest run cannot
 * collide. Caller uses it immediately; the file is unlinked at the end. */
static const char *scratch(void) {
    static char path[256];
    const char *dir = getenv("TMPDIR");
    snprintf(path, sizeof(path), "%s/kdeskdash-test-state-%d",
             (dir && dir[0]) ? dir : "/tmp", (int)getpid());
    return path;
}

static bool file_bytes(const char *path, char *out, size_t outsz, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    size_t n = fread(out, 1, outsz - 1, f);
    fclose(f);
    out[n] = '\0';
    if (len)
        *len = n;
    return true;
}

int main(void) {
    /* ---- defaults ---- */
    {
        panel_state_t st;
        panel_state_defaults(&st);
        eq_long(st.golz_human_wins, PANEL_STATE_UNSET, "default human_wins unset");
        eq_long(st.golz_zombie_wins, PANEL_STATE_UNSET, "default zombie_wins unset");
        eq_long(st.golz_ties, PANEL_STATE_UNSET, "default ties unset");
        eq_long(st.golz_gens_to_win, PANEL_STATE_UNSET, "default gens_to_win unset");
        eq_long(st.golz_wins, PANEL_STATE_UNSET, "default legacy wins unset");
        eq_str(st.calc_regs, "", "default calc regs empty");
        eq_str(st.dev_left, "", "default dev left empty");
        eq_str(st.dev_right, "", "default dev right empty");
        eq_str(st.active_mode, "", "default active mode empty");
    }

    /* ---- a full file parses ---- */
    {
        panel_state_t st;
        check(parse("# kdeskdash panel state\n"
                    "\n"
                    "golz.human_wins=41\n"
                    "golz.zombie_wins=7\n"
                    "golz.ties=2\n"
                    "golz.gens_to_win=420\n"
                    "golz.wins=13883\n"
                    "calc.regs=0:1.4142135623730951 3:-1.5\n"
                    "dev.left=kai\n"
                    "dev.right=kubs0\n"
                    "active_mode=golz\n",
                    &st),
              "a full file parses");
        eq_long(st.golz_human_wins, 41, "human_wins");
        eq_long(st.golz_zombie_wins, 7, "zombie_wins");
        eq_long(st.golz_ties, 2, "ties");
        eq_long(st.golz_gens_to_win, 420, "gens_to_win");
        eq_long(st.golz_wins, 13883, "legacy wins");
        eq_str(st.calc_regs, "0:1.4142135623730951 3:-1.5", "calc regs");
        eq_str(st.dev_left, "kai", "dev left");
        eq_str(st.dev_right, "kubs0", "dev right");
        eq_str(st.active_mode, "golz", "active mode");
    }

    /* ---- absent fields stay unset; a partial file is legitimate ---- */
    {
        panel_state_t st;
        check(parse("active_mode=clock\n", &st), "a one-line file parses");
        eq_str(st.active_mode, "clock", "the field present");
        eq_long(st.golz_human_wins, PANEL_STATE_UNSET,
                "a field the file omits stays unset");
        eq_str(st.calc_regs, "", "an omitted string stays empty");
    }
    {
        panel_state_t st;
        check(parse("", &st), "an empty file parses to defaults");
        check(parse("\n\n# only comments\n\n", &st),
              "comments and blanks alone parse");
        eq_long(st.golz_ties, PANEL_STATE_UNSET, "still unset");
    }
    {
        /* No trailing newline on the last line. */
        panel_state_t st;
        check(parse("active_mode=calc", &st), "a last line without \\n parses");
        eq_str(st.active_mode, "calc", "and its value is read");
    }

    /* ---- whole-file rejection ---- */
    {
        panel_state_t st;
        check(!parse("active_mode=clock\nthis line has no equals sign\n", &st),
              "a line with no '=' rejects the file");
        eq_str(st.active_mode, "",
               "and the fields read before it are NOT kept (whole-file rule)");
    }
    {
        panel_state_t st;
        check(!parse("golz.ties=not-a-number\n", &st),
              "an unparseable counter rejects the file");
        check(!parse("golz.ties=12x\n", &st),
              "a counter with trailing junk rejects the file");
        check(!parse("golz.ties=\n", &st),
              "an empty counter value rejects the file");
        check(!parse("golz.ties=-1\n", &st),
              "a negative counter rejects the file (UNSET is not a value)");
        check(!parse("=42\n", &st), "an empty key rejects the file");
        eq_long(st.golz_ties, PANEL_STATE_UNSET, "nothing survives a rejection");
    }
    {
        /* Over-length values are rejected, never truncated into place — a
         * truncated host token or register line is a value nobody wrote. */
        panel_state_t st;
        char big[PANEL_STATE_HOST_MAX + 64];
        int n = snprintf(big, sizeof(big), "dev.left=");
        memset(big + n, 'a', PANEL_STATE_HOST_MAX);
        big[n + PANEL_STATE_HOST_MAX] = '\n';
        big[n + PANEL_STATE_HOST_MAX + 1] = '\0';
        check(!parse(big, &st), "an over-length value rejects the file");
        eq_str(st.dev_left, "", "and is not truncated into place");
    }
    {
        /* One byte under the limit is fine — the boundary, not just past it. */
        panel_state_t st;
        char ok[PANEL_STATE_HOST_MAX + 64];
        int n = snprintf(ok, sizeof(ok), "dev.left=");
        memset(ok + n, 'a', PANEL_STATE_HOST_MAX - 1);
        ok[n + PANEL_STATE_HOST_MAX - 1] = '\n';
        ok[n + PANEL_STATE_HOST_MAX] = '\0';
        check(parse(ok, &st), "a value at the maximum length is accepted");
        check(strlen(st.dev_left) == PANEL_STATE_HOST_MAX - 1,
              "and arrives whole");
    }

    /* ---- an unknown key is the ONE thing that does not reject ---- */
    {
        panel_state_t st;
        check(parse("active_mode=dev\n"
                    "golz.future_counter=9\n"
                    "something.else=whatever it likes\n",
                    &st),
              "an unknown key is ignored, not rejected (rollback)");
        eq_str(st.active_mode, "dev", "and the keys around it still apply");
    }

    /* ---- serialize omits unset, and round-trips ---- */
    {
        panel_state_t st;
        char text[PANEL_STATE_TEXT_MAX];
        panel_state_defaults(&st);
        size_t n = panel_state_serialize(&st, text, sizeof(text));
        check(n > 0, "an empty state still serializes (the header line)");
        check(strstr(text, "golz.") == NULL, "unset counters are omitted");
        check(strstr(text, "active_mode") == NULL, "empty strings are omitted");

        panel_state_t back;
        check(parse(text, &back), "the empty serialization parses");
        eq_long(back.golz_ties, PANEL_STATE_UNSET, "and round-trips as unset");
    }
    {
        panel_state_t st;
        panel_state_defaults(&st);
        st.golz_human_wins = 41;
        st.golz_zombie_wins = 0; /* zero is a real count, not "unset" */
        st.golz_gens_to_win = 420;
        snprintf(st.calc_regs, sizeof(st.calc_regs), "0:1.5 9:-2");
        snprintf(st.dev_left, sizeof(st.dev_left), "kai");
        snprintf(st.active_mode, sizeof(st.active_mode), "golz");

        char text[PANEL_STATE_TEXT_MAX];
        check(panel_state_serialize(&st, text, sizeof(text)) > 0, "serializes");

        panel_state_t back;
        check(parse(text, &back), "the serialization parses");
        eq_long(back.golz_human_wins, 41, "round-trip human_wins");
        eq_long(back.golz_zombie_wins, 0, "round-trip a ZERO counter");
        eq_long(back.golz_ties, PANEL_STATE_UNSET, "round-trip an unset counter");
        eq_long(back.golz_gens_to_win, 420, "round-trip gens_to_win");
        eq_str(back.calc_regs, "0:1.5 9:-2", "round-trip calc regs (spaces kept)");
        eq_str(back.dev_left, "kai", "round-trip dev left");
        eq_str(back.dev_right, "", "round-trip an empty string");
        eq_str(back.active_mode, "golz", "round-trip active mode");
    }
    {
        /* Too small a buffer fails rather than writing a half file. */
        panel_state_t st;
        char tiny[8];
        panel_state_defaults(&st);
        snprintf(st.active_mode, sizeof(st.active_mode), "golz");
        check(panel_state_serialize(&st, tiny, sizeof(tiny)) == 0,
              "serialize into too small a buffer fails");
    }

    /* ---- load / atomic save ---- */
    {
        const char *path = scratch();
        char tmp[320];
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        unlink(path);
        unlink(tmp);

        panel_state_t st;
        check(panel_state_load(path, &st) == PANEL_STATE_ABSENT,
              "a missing file loads as ABSENT (the migration case)");
        eq_long(st.golz_ties, PANEL_STATE_UNSET, "and yields defaults");

        panel_state_defaults(&st);
        st.golz_human_wins = 5;
        snprintf(st.active_mode, sizeof(st.active_mode), "calc");
        check(panel_state_save(path, &st), "save writes");
        check(access(tmp, F_OK) != 0,
              "the temp file is gone after a successful save");

        panel_state_t back;
        check(panel_state_load(path, &back) == PANEL_STATE_OK, "load reads back");
        eq_long(back.golz_human_wins, 5, "and the value survived the round trip");
        eq_str(back.active_mode, "calc", "as did the string");

        /* A rejected file is INVALID, not ABSENT — the distinction that stops a
         * corrupt file silently re-migrating stale values out of the old Redis. */
        FILE *f = fopen(path, "wb");
        check(f != NULL, "reopen scratch for the malformed case");
        if (f) {
            fputs("golz.ties=nope\n", f);
            fclose(f);
        }
        check(panel_state_load(path, &back) == PANEL_STATE_INVALID,
              "a malformed file loads as INVALID, never ABSENT");
        eq_long(back.golz_ties, PANEL_STATE_UNSET, "and yields defaults");

        /* A failed save leaves the previous target untouched. An unwritable
         * directory is the reachable failure (ProtectSystem=strict makes every
         * path but the state directory exactly this). */
        panel_state_defaults(&st);
        st.golz_human_wins = 77;
        check(panel_state_save(path, &st), "re-save a known-good file");
        char before[PANEL_STATE_FILE_MAX];
        size_t before_len = 0;
        check(file_bytes(path, before, sizeof(before), &before_len),
              "read the known-good bytes");

        panel_state_t bad;
        panel_state_defaults(&bad);
        bad.golz_human_wins = 99;
        check(!panel_state_save("/proc/kdeskdash-nonexistent/state", &bad),
              "a save into an unwritable directory fails");
        char after[PANEL_STATE_FILE_MAX];
        size_t after_len = 0;
        check(file_bytes(path, after, sizeof(after), &after_len),
              "the previous file is still readable");
        check(before_len == after_len && memcmp(before, after, before_len) == 0,
              "and is byte-identical — a failed save clobbers nothing");

        unlink(path);
        unlink(tmp);
    }

    /* ---- an oversize file is refused rather than read into a bigger buffer ---- */
    {
        const char *path = scratch();
        FILE *f = fopen(path, "wb");
        check(f != NULL, "open scratch for the oversize case");
        if (f) {
            for (size_t i = 0; i < PANEL_STATE_FILE_MAX + 64; i++)
                fputc('#', f);
            fputc('\n', f);
            fclose(f);
        }
        panel_state_t st;
        check(panel_state_load(path, &st) == PANEL_STATE_INVALID,
              "an oversize file is INVALID");
        unlink(path);
    }

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_panel_state: all passed\n");
    return 0;
}
