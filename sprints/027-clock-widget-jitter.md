# 027 — Clock widget: stop the once-a-second HH:MM twitch

WI #1149 · proposal korg:1150

## Goal

The Launcher's clock pane twitched. Ken's report was precise enough to be
half a diagnosis on its own: the last seconds digit cycles 0-9 every ten
seconds, and the 0→1 and 1→2 transitions shift the *whole* local time, because
it is centered and its width is changing.

## The mechanism

`clock_widget.c` builds the local face as a content-sized flex row —
`[HH:MM (48px)][:SS (28px)]` — inside a centered column. Montserrat is a
proportional font: `1` is materially narrower than `0`. So the seconds label's
width changes as the digits tick, the row it lives in changes width, and
because the row is centered **half of every seconds-width change is handed
straight to HH:MM as horizontal displacement**.

Which is why the 0→1 and 1→2 transitions are the ones you notice: they are the
widest-to-narrowest digit changes in the font.

## What shipped

One helper in [src/clock_widget.c](../src/clock_widget.c), `pin_digit_pair_width`,
plus one call site. It finds the widest digit in the face's font, measures
`":DD"` at that digit, pins the seconds label to that width and left-aligns it.
The seconds digits then shuffle inside a box that does not move, growing
rightward into empty background.

**The widest digit is found, not assumed.** `'0'` is the usual answer and it is
a per-font fact; hardcoding it would silently reintroduce the wander for exactly
the digits where it is worst, in a way no test here could catch.

## Measured, before and after

This is a "does it look better" bug, which is the kind most easily declared
fixed without evidence. So it was measured instead: `kddss` frames off rpidash2
in Launcher mode, with the left edge of the big HH:MM glyph band extracted from
each PNG.

| | HH:MM left edge |
|---|---|
| **Before**, seconds ticking | 5 px spread, 4 distinct positions |
| **After**, 11 frames within one minute (`:37`–`:58`) | **1562 px on every frame — 0 px** |
| **After**, frame crossing the minute rollover | 1 px |

Eleven consecutive frames landing on the same pixel is the result the WI asked
for. The residual 1 px is the HH:MM digits' *own* width change at the minute
boundary — option 3's stated, accepted tradeoff, and not what the eye locks onto.

The frames were timestamped from the board (`date +%H%M%S` immediately before
each capture) rather than assumed to be a second apart, because the whole claim
is "seconds contribute nothing, minutes contribute a little" and that is not
separable without knowing which frame crossed a minute.

## Decisions

**Why not the other two options in #1149.** Both wanted a monospaced font, and
there isn't one in this build. Montserrat is proportional, and the vendored
`SymbolsNerdFont-Regular.ttf` has zero Latin glyphs — the same fact
`docs/solutions/best-practices/draw-only-glyphs-the-font-has.md` records for a
different reason. Either option therefore means a new vendored asset plus a
TinyTTF face, which is a real piece of work and changes how the clock looks.
Pinning the box costs no asset and no visual change at all.

**The residual is left in.** Removing the minute-boundary pixel needs genuinely
tabular digits, i.e. the font work above. Fixing the HH:MM label's width instead
would only move the problem: the glyphs would still shuffle inside their own
box, at the same magnitude, for the same reason.

**The UTC row is deliberately untouched.** It has the same class of issue one
level down — its `UTC` caption shifts when `utc_hm` changes width — but only at
minute rate, which is the rate this sprint just decided is acceptable. Fixing it
would be one more call to the same helper; it is left alone because nothing
reported it, not because it was missed.

`clock_widget` is shared, so this lands in the `clock` mode rebuild (#1136) for
free when that happens.

## Found while shipping: the stopwatch has the same bug, faster

`clock` mode positions its stopwatch readout with
`lv_obj_align(st->sw_label, LV_ALIGN_RIGHT_MID, -60, -50)`. No flex involved,
same family of failure: the **right** edge is pinned, so as the digits change
width they push the **left** edge around — and that readout updates at 10 Hz,
ten times the rate that was distracting enough to file #1149.

Not fixed here — it is outside this WI's scope, and #1136 rebuilds `clock` mode
on `clock_widget` anyway, which is the natural moment. Recorded in
[docs/solutions/best-practices/proportional-digits-move-their-neighbours.md](../docs/solutions/best-practices/proportional-digits-move-their-neighbours.md)
along with the general check: when a readout changes on a timer, ask which edge
is pinned and which one the digits push.

## Verified on hardware

Iterated on rpidash2 via `just push-dev`, reviewed live by Ken, and measured as
above. The board must be returned to a published version at ship time — see the
deploy note below.

## Deployed 2026-08-09

Published from merged `main` (`b316406`) and installed on the whole fleet. Both
boards reachable.

- **Artifact:** `0.27.0-b316406` (store `latest`). `VERSION` → `0.27.0`, the
  minor tracking the sprint number as usual.
- **Rollback target:** `0.26.0-508d5e4` — `just deploy <host> 0.26.0-508d5e4`.
- **rpidash2** (Pi 5, dev desk) — `kdeskdash 0.27.0-b316406`, unit active. This
  board had been sitting on the `0.26.0-39221a3-dirty` push-dev build since the
  prototype; the deploy is what returned it to a published version.
- **rpidash3** (Pi 4, work desk) — `kdeskdash 0.27.0-b316406`, unit active.

**Re-measured against the shipped artifact, not the dev build.** The numbers
above came from a `push-dev` binary, which is not what ships, so the check was
run again on the published one:

| | HH:MM left edge |
|---|---|
| `0.27.0-b316406`, 10 frames within one minute | **1557 px on every frame — 0 px, one distinct position** |

(The absolute value differs from the prototype run only because the time itself
differed — `02:20` and `21:36` are not the same width. Stability is the claim,
not the coordinate.) The right edge still ranges 1715–1727 across those frames,
which is the seconds digits growing and shrinking *inside* their pinned box —
the intended behaviour, and the evidence that the box is doing the work rather
than the text having stopped changing.

**One observation, pre-existing — and it turns out to be a feature.** rpidash3
came back showing Launcher's `no launcher configured` empty state with the clock
pane on the *left* rather than its usual right-hand 30%: with the grid hidden,
it is no longer being pushed right by anything. I flagged it as a possible
layout bug.

It is not. Ken has already learned to read it as **"kwork hasn't attached
yet"** — a whole-pane position change is legible across the room in a way the
small grey banner underneath it is not. So the accident is load-bearing, and
the danger is that it looks exactly like something a future tidy-up would
quietly correct. `launcher.c`'s empty-state branch now says so at the point
where someone would make that change, with the rule attached: replace the
signal before removing it.
