/**
 * @file claude_view.c
 * Implementation of the claude mode's panel-side rendering helpers. See
 * claude_view.h for why these did not move into libkdash with everything else.
 */
#include "modes/claude_view.h"

#include <stdio.h>
#include <time.h>

const char *claude_disp_label(kdash_claude_disp_t d) {
    switch (d) {
    case KDASH_CLAUDE_DISP_BLOCKED:  return "BLOCKED ON YOU";
    case KDASH_CLAUDE_DISP_AWAITING: return "AWAITING INPUT";
    case KDASH_CLAUDE_DISP_WORKING:  return "WORKING";
    case KDASH_CLAUDE_DISP_IDLE:     return "IDLE";
    case KDASH_CLAUDE_DISP_STALE:    return "STALE";
    }
    return "?";
}

const char *claude_project_label(const char *project) {
    return (project && project[0] != '\0') ? project : "?";
}

void claude_fmt_age(long long age_s, char *out, size_t outsz) {
    if (!out || outsz == 0)
        return;
    if (age_s < 0)
        age_s = 0;
    if (age_s < 60)
        snprintf(out, outsz, "%llds", age_s);
    else if (age_s < 3600)
        snprintf(out, outsz, "%lldm", age_s / 60);
    else if (age_s < 86400)
        snprintf(out, outsz, "%lldh", age_s / 3600);
    else
        snprintf(out, outsz, "%lldd", age_s / 86400);
}

void claude_fmt_reset(long long resets_at, long long now, char *out,
                      size_t outsz) {
    if (!out || outsz == 0)
        return;
    if (resets_at <= 0) {
        snprintf(out, outsz, "--");
        return;
    }
    time_t t = (time_t)resets_at;
    struct tm tm;
    if (!localtime_r(&t, &tm)) {
        snprintf(out, outsz, "--");
        return;
    }
    /* Same-ish day (< 18h out): bare clock. Further: prefix the weekday. */
    if (resets_at - now < 18 * 3600)
        strftime(out, outsz, "%H:%M", &tm);
    else
        strftime(out, outsz, "%a %H:%M", &tm);
}
