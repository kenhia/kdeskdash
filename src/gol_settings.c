/**
 * @file gol_settings.c
 * Pure, host-testable validation of untrusted GoL settings-hash fields.
 * See gol_settings.h. No hiredis / LVGL dependency.
 */
#include "gol_settings.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Strict base-10 integer parse. The whole token must be an integer with only
 * leading/trailing ASCII whitespace; empty, non-numeric ("abc"), trailing junk
 * ("3.9", "12px"), and overflowing values all fail. This is stricter than the
 * previous atoi(), which silently coerced junk to 0 — the point of extracting
 * this validation boundary. Returns false without touching *out on failure. */
static bool parse_int(const char *s, long *out) {
    if (!s)
        return false;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '\0')
        return false;
    errno = 0;
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s)
        return false; /* no digits consumed */
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')
        end++;
    if (*end != '\0')
        return false; /* trailing junk */
    if (errno == ERANGE)
        return false;
    *out = v;
    return true;
}

/* Strict float parse, same discipline as parse_int (replaces atof()). */
static bool parse_double(const char *s, double *out) {
    if (!s)
        return false;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '\0')
        return false;
    errno = 0;
    char *end;
    double v = strtod(s, &end);
    if (end == s)
        return false;
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')
        end++;
    if (*end != '\0')
        return false;
    if (errno == ERANGE)
        return false;
    *out = v;
    return true;
}

bool gol_settings_apply_field(gol_settings_t *cfg, const char *field,
                              const char *val) {
    if (!cfg || !field || !val)
        return false;

    long iv;
    double dv;

    if (strcmp(field, "cell_size") == 0) {
        if (parse_int(val, &iv) && iv >= 2 && iv <= 64) {
            cfg->cell_size = (int)iv;
            return true;
        }
    } else if (strcmp(field, "padding") == 0) {
        if (parse_int(val, &iv) && iv >= 0 && iv <= 16) {
            cfg->padding = (int)iv;
            return true;
        }
    } else if (strcmp(field, "density") == 0) {
        if (parse_double(val, &dv) && dv > 0.0 && dv <= 1.0) {
            cfg->density = dv;
            return true;
        }
    } else if (strcmp(field, "trail") == 0) {
        if (parse_int(val, &iv)) {
            cfg->trail = iv != 0;
            return true;
        }
    } else if (strcmp(field, "trail_turns") == 0) {
        if (parse_int(val, &iv) && iv >= 1 && iv <= 64) {
            cfg->trail_turns = (int)iv;
            return true;
        }
    } else if (strcmp(field, "speed_ms") == 0) {
        if (parse_int(val, &iv) && iv >= 10 && iv <= 5000) {
            cfg->speed_ms = (int)iv;
            return true;
        }
    } else if (strcmp(field, "rgb") == 0) {
        if (parse_int(val, &iv)) {
            cfg->rgb = iv != 0;
            return true;
        }
    }
    return false;
}
