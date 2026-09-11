/**
 * @file claude_view.h
 * Pure, host-testable rendering helpers for the `claude` mode (no LVGL, no
 * Redis). Deliberately NOT a feed reader: the feed's contract — key grammar,
 * hash parsing, the display-status ladder, the attention-first sort, limits
 * staleness — belongs to libkdash (`kdash/kdash_feed.h`, `kdash_payload.h`)
 * since sprint 034, and this panel derives none of it a second time.
 *
 * What is left here is what kdashdata's CD-10 deliberately keeps on the panel:
 * display strings and the placeholder for an absent field. libkdash hands back
 * the enum's own lowercase name ("blocked") and an empty `project`; the words
 * on this 1920x440 panel ("BLOCKED ON YOU", "?") are this project's choice and
 * nobody else's.
 */
#ifndef KDESKDASH_MODES_CLAUDE_VIEW_H
#define KDESKDASH_MODES_CLAUDE_VIEW_H

#include <stddef.h>

#include "kdash/kdash_payload.h"

/* Usage arc switches to the warning treatment at this utilisation. */
#define CLAUDE_LIMITS_WARN_PCT 80.0

/* Fixed uppercase panel label for a display state ("BLOCKED ON YOU", ...).
 * The panel's vocabulary, not the schema's — kdash_claude_disp_str() hands
 * back the enum name and says so. */
const char *claude_disp_label(kdash_claude_disp_t d);

/* The project name to render. libkdash supplies no placeholder for an absent
 * project (CD-10: a "?" is rendering), so the substitution happens here.
 * Never returns NULL. */
const char *claude_project_label(const char *project);

/* Compact age string: "12s", "3m", "2h", "5d". Negative clamps to "0s". */
void claude_fmt_age(long long age_s, char *out, size_t outsz);

/* Wall-clock reset formatting (localtime): within ~18h -> "14:00", further out
 * -> "Tue 07:00", unknown (<= 0) -> "--". */
void claude_fmt_reset(long long resets_at, long long now, char *out,
                      size_t outsz);

#endif /* KDESKDASH_MODES_CLAUDE_VIEW_H */
