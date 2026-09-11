# Tap and hold on one widget: `SHORT_CLICKED`, never `CLICKED`

**Sprint 036 (WI #509), `src/modes/calc.c`.**

Giving one widget two gestures — tap does one thing, hold does another — is the
cheapest way to buy screen space back from a pair of buttons. On this panel it
paid for two whole keypad columns. It has one trap, and it is silent.

## The trap

LVGL sends `LV_EVENT_CLICKED` **on every release**, whether or not
`LV_EVENT_LONG_PRESSED` already fired during the press. From
`lib/lvgl/src/indev/lv_indev.c`, the release path:

```c
if(i->long_pr_sent == 0 && is_enabled) {
    send_event(LV_EVENT_SHORT_CLICKED, indev_act);   /* gated */
}
if(is_enabled) {
    send_event(LV_EVENT_CLICKED, indev_act);         /* always */
}
```

So the obvious wiring —

```c
lv_obj_add_event_cb(row, tap_cb,  LV_EVENT_CLICKED,      ref);  /* WRONG */
lv_obj_add_event_cb(row, hold_cb, LV_EVENT_LONG_PRESSED, ref);
```

— fires **both** on a hold: `hold_cb` mid-press, then `tap_cb` on release. In
the calculator that meant every store was immediately followed by a recall of
the same row, so holding a register appeared to do nothing at all. Nothing
errors, nothing logs; the feature is just inert.

## The fix

`LV_EVENT_SHORT_CLICKED` is already the event gated on `long_pr_sent == 0`.
It is exactly the "tap that was not a hold" that this gesture pair needs:

```c
lv_obj_add_event_cb(row, tap_cb,  LV_EVENT_SHORT_CLICKED, ref);
lv_obj_add_event_cb(row, hold_cb, LV_EVENT_LONG_PRESSED,  ref);
```

No suppression flag on the widget, no timestamp comparison, no state to get
wrong. LVGL already made the distinction; the job is to read the right event.

## The swipe guard still applies — and it does work here

Every handler inside this shell needs the swipe-vs-tap guard
([lvgl-swipe-vs-tap-gesture-guard.md](lvgl-swipe-vs-tap-gesture-guard.md)), and
the obvious worry is that `LONG_PRESSED` fires *mid-press*, before release — so
is the gesture even recognised yet?

It is, and this was checked rather than assumed. In `indev_proc_press()`,
`indev_gesture()` runs **earlier in the same iteration** than the long-press
timer check that sends `LONG_PRESSED`. `lv_indev_get_gesture_dir()` is therefore
already set for a swipe that has passed `gesture_limit`, so the ordinary

```c
if (is_swipe()) return;
```

guard covers the hold as well as the tap. A slow swipe that lingers over the
widget is a gesture before it is ever a hold.

## Two design notes, since the gesture is invisible

- **Say what it does.** Two labelled buttons were discoverable; a hold is not.
  The register panel carries a `tap RCL / hold STO` hint line. ASCII only —
  Montserrat has no U+00B7, and a separator that draws as a box is the bug
  [draw-only-glyphs-the-font-has.md](draw-only-glyphs-the-font-has.md) is about.
- **Size the target for the harder gesture.** A hold that misses does not do
  nothing — it falls through to the tap, which here meant *recalling* when the
  user meant to *store*, overwriting the display instead of the register. That
  asymmetry is why the calculator kept six registers rather than eight: six
  rows are 60 px (7.8 mm) tall in the 424 px panel, eight would be 45 px
  (5.9 mm). When the two gestures have different consequences, the target is
  sized for the one you cannot afford to miss.
