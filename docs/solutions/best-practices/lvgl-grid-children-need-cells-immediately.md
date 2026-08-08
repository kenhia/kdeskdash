---
title: LVGL grid children need a cell before the first layout pass — hidden ones too
date: 2026-08-08
category: docs/solutions/best-practices
module: LVGL UI modes (launcher)
problem_type: best_practice
component: frontend_stimulus
severity: high
applies_when:
  - Using LV_LAYOUT_GRID with a pool of pre-created children
  - The grid's rows/cols are not known until data arrives
  - Children are created hidden and revealed later
related_components:
  - launcher mode
tags: [lvgl, grid, layout, segfault, embedded-ui]
---

# LVGL grid children need a cell before the first layout pass

## The failure

A mode pre-creates a pool of buttons so that later refreshes only reposition
them instead of allocating. The grid dimensions come off a feed, so the track
descriptors are installed on the first successful refresh. Every button starts
`LV_OBJ_FLAG_HIDDEN`, because there is nothing to show yet.

On the panel this segfaults immediately — before any data arrives — in a systemd
restart loop, preceded by:

```
[Warn] calc_rows: No row descriptor found even on the parent  lv_grid.c:377
[Warn] calc_cols: No col descriptor found even on the parent  lv_grid.c:285
```

It does not reproduce on the host, because the host build has no display and
never runs a layout pass.

## Why

Two assumptions, both wrong:

1. **Hidden children are not laid out.** They are. `LV_OBJ_FLAG_HIDDEN` affects
   drawing, not the layout walk.
2. **A grid with no children to place needs no tracks.** The layout runs as soon
   as the object tree is realised — `lv_obj_update_layout()`, or the first
   screen load — and dereferences the descriptor arrays whether or not anything
   is visible.

So a grid parent with no `lv_obj_set_grid_dsc_array`, or a grid child with no
`lv_obj_set_grid_cell`, is a null dereference waiting for the first paint.

## The fix

Make the grid well-formed from the moment it exists, then overwrite it when the
real dimensions arrive:

```c
/* placeholder 1x1 track set at build time */
st->col_dsc[0] = LV_GRID_FR(1);  st->col_dsc[1] = LV_GRID_TEMPLATE_LAST;
st->row_dsc[0] = LV_GRID_FR(1);  st->row_dsc[1] = LV_GRID_TEMPLATE_LAST;
lv_obj_set_grid_dsc_array(grid, st->col_dsc, st->row_dsc);

/* and every pooled child gets a valid cell as it is created */
lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, 0, 1,
                          LV_GRID_ALIGN_STRETCH, 0, 1);
```

A 1×1 placeholder is not a layout assumption — it is just a legal grid. The real
rows and cols still come entirely from the feed.

## Also worth knowing

**LVGL stores the descriptor arrays by pointer, not by value.** They must outlive
every layout pass, so they belong in the mode's state struct, never on the stack
of the function that installs them.

## The general shape

This is the class of bug the host test suite structurally cannot catch: pure
cores are tested on the build host, and LVGL glue only runs where there is a
display. When a change touches layout, `just push-dev <host>` and look at the
panel — the board is the only place this failure exists.
