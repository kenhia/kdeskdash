---
title: Distinguishing swipe gestures from taps on LVGL CLICKED handlers
date: 2026-06-09
category: docs/solutions/best-practices
module: LVGL UI modes (shell, content modes)
problem_type: best_practice
component: frontend_stimulus
severity: medium
applies_when:
  - Adding an LVGL CLICKED handler to a button or canvas inside the swipe-navigated shell
  - A widget sits on a screen whose parent handles LV_EVENT_GESTURE for navigation
  - A tap's target region depends on where the press landed
related_components:
  - shell
  - menu mode
  - game_of_life mode
tags: [lvgl, gesture, tap, swipe, touch, event-handling, embedded-ui]
---

# Distinguishing swipe gestures from taps on LVGL CLICKED handlers

## Context

kdeskdash is a touch dashboard whose shell navigates between modes with swipe
gestures (`LV_EVENT_GESTURE` on the screen): swipe left/right cycles content
modes, swipe down opens the menu. Individual modes also place tappable widgets
(menu tiles, control buttons, a full-screen canvas) that act on
`LV_EVENT_CLICKED`.

The friction: **a swipe that begins on a clickable widget still releases over
that widget, so LVGL fires `LV_EVENT_CLICKED` on release** — even though the
user meant to swipe. Left unguarded, a swipe-down that happens to start on a
menu tile launches that tile's mode instead of opening the menu. This pattern
has now bitten twice — first in the menu launcher (`menu.c` tile taps), then
again in the Game of Life tap-menu (`game_of_life.c`) — which is why it is worth
documenting as institutional knowledge.

## Guidance

In every `LV_EVENT_CLICKED` callback on a swipe-navigated screen, bail out when
the active input device reports an in-progress gesture. Guard the input-device
pointer for NULL **before** dereferencing it:

```c
static void tile_cb(lv_event_t *e) {
    /* A swipe that starts on a tile still releases over it, so LVGL would fire
     * CLICKED here. If the touch was actually a gesture (e.g. swipe-down to the
     * menu), ignore it so only genuine taps launch a mode. */
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE)
        return;

    kd_mode_t *target = lv_event_get_user_data(e);
    if (target)
        shell_set_active(target);
}
```

Two non-obvious requirements make this work:

1. **Set `LV_OBJ_FLAG_GESTURE_BUBBLE` on the widget (and its children).** A
   clickable object consumes the gesture by default; without
   `GESTURE_BUBBLE` the swipe never reaches the shell's screen-level
   `LV_EVENT_GESTURE` handler, so navigation silently stops working over your
   widget. Add the flag to the panel, every button, and any label inside it.

2. **For region-sensitive taps, capture the press point on `LV_EVENT_PRESSED`
   and decide on `LV_EVENT_CLICKED`.** At press time the gesture detector has
   not yet classified the touch, so `lv_indev_get_gesture_dir()` is not
   meaningful. Record the coordinate on PRESSED, then apply both the gesture
   guard and the region test on CLICKED:

   ```c
   static void canvas_pressed_cb(lv_event_t *e) {
       gol_mode_state_t *st = lv_event_get_user_data(e);
       lv_indev_t *indev = lv_indev_active();
       if (indev)
           lv_indev_get_point(indev, &st->press_pt);
   }

   static void canvas_clicked_cb(lv_event_t *e) {
       gol_mode_state_t *st = lv_event_get_user_data(e);
       lv_indev_t *indev = lv_indev_active();
       if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE)
           return;                       /* swipe drives shell nav, not the tap */
       if (st->press_pt.x >= st->disp_w * 3 / 4)
           open_menu(st);                /* only taps in the right quarter act */
   }
   ```

Plain helper functions invoked *from* a guarded callback (e.g. a `close_menu()`
that the button callbacks call) do not need their own guard — they are not event
callbacks and run only after the guard upstream has already passed.

## Why This Matters

- **Correctness:** Without the guard, gesture and tap handling collide — swipes
  that originate on a widget trigger unintended actions, and the shell's
  navigation feels broken in exactly the spots users touch most.
- **NULL-safety:** `lv_indev_get_gesture_dir()` dereferences its argument with
  no NULL check (see `lib/lvgl/src/indev/lv_indev.c`), and `lv_indev_active()`
  returns NULL when no input device is currently being processed (for example a
  programmatically dispatched `CLICKED`). Passing the result straight through —
  `lv_indev_get_gesture_dir(lv_indev_active())` — is a latent NULL dereference.
  A code review of the Game of Life tap menu caught three button callbacks doing
  exactly this; the touch-only panel never hit it in practice, but the
  inconsistency is a real crash waiting for a non-touch input path.
- **Consistency:** Once one callback on a screen needs the guard, all of them
  do. A single unguarded callback is the bug.

## When to Apply

- Adding any `LV_EVENT_CLICKED` handler to a widget that lives on a
  swipe-navigated screen.
- The widget's parent screen registers `LV_EVENT_GESTURE` for navigation.
- A tap's effect depends on *where* the press landed (use the
  PRESSED-capture / CLICKED-decide split).

Not needed for widgets on screens with no gesture navigation, or for non-event
helper functions.

## Examples

**Before (buggy — swipe launches the wrong action, and unguarded NULL):**

```c
static void reset_cb(lv_event_t *e) {
    gol_mode_state_t *st = lv_event_get_user_data(e);
    /* fires even on a swipe that started here; also derefs a possibly-NULL indev */
    if (lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_NONE)
        return;
    reseed_same(st);
    close_menu(st);
}
```

**After (guarded + NULL-safe):**

```c
static void reset_cb(lv_event_t *e) {
    gol_mode_state_t *st = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE)
        return;
    reseed_same(st);
    close_menu(st);
}
```

**Enabling swipe pass-through on the overlay and its children:**

```c
lv_obj_add_flag(panel, LV_OBJ_FLAG_GESTURE_BUBBLE);  /* panel */
lv_obj_add_flag(btn,   LV_OBJ_FLAG_GESTURE_BUBBLE);  /* each button */
lv_obj_add_flag(info,  LV_OBJ_FLAG_GESTURE_BUBBLE);  /* and labels */
```

## Related

- `src/modes/menu.c` — `tile_cb` (canonical example with the explanatory comment)
- `src/modes/game_of_life.c` — `canvas_pressed_cb` / `canvas_clicked_cb`
  (PRESSED-capture / CLICKED-decide split) and `reset_cb` / `restart_cb` /
  `cancel_cb` (button guards)
- `src/shell.c` — screen-level `LV_EVENT_GESTURE` navigation that
  `GESTURE_BUBBLE` must reach
