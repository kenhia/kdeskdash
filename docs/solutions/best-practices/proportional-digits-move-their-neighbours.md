---
title: A ticking readout in a proportional font moves whatever it is anchored to
date: 2026-08-09
category: docs/solutions/best-practices
problem_type: best_practice
module: clock_widget, clock mode (stopwatch), any live numeric readout
component: ui
applies_when:
  - A label's text changes on a timer (clock, stopwatch, metrics, percentages)
  - That label shares a centered LV_SIZE_CONTENT flex row with something else
  - Or it is positioned with lv_obj_align from an edge other than its own growth direction
  - The font is Montserrat, or any other proportional face
tags: [lvgl, layout, fonts, clock, jitter, polish]
---

# A ticking readout in a proportional font moves whatever it is anchored to

## Context

Sprint 027 (WI #1149). The Launcher's clock pane twitched once a second. Ken's
report contained the diagnosis: the last seconds digit cycles 0-9, and the 0→1
and 1→2 transitions shift the *whole* local time, which is centered.

**Montserrat's digits are not tabular.** `1` is materially narrower than `0`.
Every built-in LVGL font in this project is proportional, and the vendored
`SymbolsNerdFont-Regular.ttf` has no Latin glyphs at all, so there is currently
no font in this build that can draw fixed-width digits.

## The mechanism, and why it lands on the *neighbour*

`clock_widget.c` had:

```
local_box (COLUMN, cross-align CENTER, LV_SIZE_CONTENT)
└── row (ROW, LV_SIZE_CONTENT)
    ├── HH:MM   48px
    └── :SS     28px   <- width changes every second
```

The seconds label changing width changes the *row's* width. The row is
content-sized inside a centered parent, so the row re-centers, and **half of
every seconds-width change is delivered to HH:MM as horizontal movement**. The
element that visibly moves is not the one whose text changed.

Measured on rpidash2: the HH:MM left edge visited 4 distinct positions spanning
5 px, once a second, indefinitely. Small in absolute terms, and very visible —
the eye is tuned to periodic motion in a way it is not tuned to a static 5 px
offset.

## The rule

**Pin the box, not the text.** Give the volatile label a fixed width equal to
its widest possible rendering, and align the text inside it toward the stable
element. The digits then shuffle within a box that does not move, growing into
background rather than into their neighbour.

```c
/* Widest digit found, not assumed — it is a per-font fact, and guessing wrong
 * silently reintroduces the wander for exactly the digits where it is worst. */
char widest[4] = ":00";
uint16_t best = 0;
for (char d = '0'; d <= '9'; d++) {
    uint16_t adv = lv_font_get_glyph_width(font, (uint32_t)d, 0);
    if (adv > best) { best = adv; widest[1] = widest[2] = d; }
}
lv_obj_set_width(label, lv_text_get_width(widest, 3, font,
                 lv_obj_get_style_text_letter_space(label, LV_PART_MAIN)));
lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
```

What this does **not** fix is the label's own glyphs shuffling inside its box.
That needs genuinely tabular digits — a vendored monospace font and a TinyTTF
face. Worth it only where the moving element is the one being read; here the
residual is one pixel at the minute boundary, which nobody notices.

## The same family, one anchor away

`lv_obj_align` has the identical failure mode without any flex involved. A
readout aligned by an edge it does not grow from moves as its width changes:

```c
lv_obj_align(st->sw_label, LV_ALIGN_RIGHT_MID, -60, -50);  /* clock mode's stopwatch */
```

Its right edge is pinned, so the digits push the **left** edge around — and the
stopwatch updates at 10 Hz, ten times the rate that was distracting on the
clock. Not fixed in sprint 027 (out of its WI's scope, and #1136 rebuilds clock
mode anyway), but it is the same bug and the same fix applies.

**The check to run when adding any live readout**: if the text changes on a
timer, ask which edge is pinned and which one the digits push. If the answer is
"the one next to something else", pin the width.
