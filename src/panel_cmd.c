/**
 * @file panel_cmd.c
 * Pure policy for commands from central: the host segment this panel answers
 * to, and the screenshot path guard. See panel_cmd.h.
 */
#include "panel_cmd.h"

#include <string.h>

/* The kdashdata host/session token charset (rules.md): [A-Za-z0-9._-].
 * Checked AFTER lowercasing, which only ever moves A-Z into a-z. */
static bool token_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool panel_cmd_host(const char *raw, char *out, size_t outsz) {
    if (!out || outsz == 0)
        return false;
    out[0] = '\0';
    if (!raw)
        return false;

    /* First label only: `rpiDash2.local` is the same machine as `rpiDash2`,
     * and central keys it by the short name. */
    size_t len = 0;
    while (raw[len] != '\0' && raw[len] != '.')
        len++;
    if (len == 0 || len > PANEL_CMD_HOST_MAX - 1)
        return false;
    if (len >= outsz)
        return false; /* fail rather than truncate: a truncated host is another panel */

    /* Validate the WHOLE label before writing a byte of it. Copying as we go
     * and bailing on the first bad character leaves `out` holding a prefix of
     * a rejected hostname — which a caller that only checks the return value
     * would never see, but one that logs `out` would print as though it meant
     * something. */
    for (size_t i = 0; i < len; i++) {
        if (!token_char(lower(raw[i])))
            return false;
    }
    for (size_t i = 0; i < len; i++)
        out[i] = lower(raw[i]);
    out[len] = '\0';
    return true;
}

bool panel_cmd_shot_path_ok(const char *path) {
    if (!path)
        return false;
    size_t len = strlen(path);
    if (len == 0 || len >= PANEL_CMD_PATH_MAX)
        return false;

    const size_t dirlen = sizeof(PANEL_CMD_SHOT_DIR) - 1;
    if (len <= dirlen || memcmp(path, PANEL_CMD_SHOT_DIR, dirlen) != 0)
        return false; /* outside the directory this panel owns — or the dir itself */
    if (path[len - 1] == '/')
        return false; /* names a directory, not a file */

    /* Reject a `..` PATH SEGMENT, not the two characters: `..shot.bmp` is an
     * oddly named file and `...` is another, while `a/../..` escapes. */
    const char *seg = path + dirlen;
    while (*seg) {
        const char *end = strchr(seg, '/');
        size_t seglen = end ? (size_t)(end - seg) : strlen(seg);
        if (seglen == 2 && seg[0] == '.' && seg[1] == '.')
            return false;
        if (!end)
            break;
        seg = end + 1;
    }
    return true;
}
