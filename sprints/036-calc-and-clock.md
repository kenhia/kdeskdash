# Sprint 036 — calc follow-ups and the clock-mode rebuild

**Proposal:** korg:2219 (program 2233, "Backlog drain 3 of 3", slice 14)
**Covers:** WI #509 (calc post-live-test follow-ups), WI #1136 (rebuild clock
mode on the shared clock widget)
**Branch:** `036-calc-and-clock`
**Posture:** overseen leg `kdeskdash-9dfbe0` — the ship is gated on the
overseer's clearance on the proposal, and the wrap-up returns as a korg handoff.

## Goal

Two M-sized features that had been sitting since July and August. Both are
self-contained UI work on the panel, neither touches a feed, and both had been
waiting on something that has since arrived — the live test for the calculator,
and the shared clock widget for the clock.

## Premise check, before any of it

Both items date from before several sprints that touched this area, so each
falsifiable claim got checked against the tree first.

**#509 holds** on every functional claim: no trig, no `√x`, no `1/x`, one
`CALC_KEY_CLEAR` with no CE, and no calc key in `redis.h`'s schema.

**#509's layout claim had drifted**, and it was the interesting one. The item
said *"Layout reserves keypad bottom-left region and spare left-panel space for
this growth."* It does not. The keypad was 7 cols × 4 rows and **all 28 cells
were occupied**, with the panel arithmetic exact in both axes — `16 + 7·113 +
6·8 = 855` in an 856 px panel, `16 + 4·96 + 3·8 = 424` in a 424 px one. There
was no reserved region to grow into. That changed *how* the sprint had to work,
not *whether*: the eight new keys had to be paid for out of somewhere.

**#1136 holds.** `src/modes/clock.c` was still a second, independent clock
implementation (its own `strftime` calls, including neither `clock_core.h` nor
`clock_widget.h`), and its stated blocker #1134 is closed.

## WI #509 — the calculator

### What shipped

Eight new keys, all of them backed by the pure core and its tests:

| Key | Behaviour |
|---|---|
| `sqrt` | `√x`. A negative argument is `Error`, never `-nan` on the display. |
| `1/x` | Reciprocal. `1/0` is `Error`. |
| `sin` `cos` `tan` | In the current angle mode. |
| `INV` | Arms the inverse for **exactly one** trig key, then disarms. |
| `DEG` / `RAD` | Toggles the angle mode, and the key's own label names it. |
| `CE` | Clears the operand being typed, keeping the pending operation. |

Plus register persistence across restarts, and the register interaction the item
had left as an open question.

### Decisions

**`INV` as a modifier, not three more keys.** #509 asked for "sin/cos/tan +
inverses", which reads as six keys. A shift modifier is the desk-calculator
idiom and costs four cells instead of six — which mattered, because the keypad
had none to spare. The modifier is consumed by the trig key it applies to, and
cleared by both `C` and `CE`, so it can never outlive the entry it was armed
for. The `INV` key lights coral while armed: an armed `INV` that looks like an
idle one turns the next trig key into a coin toss.

**`tan 90°` is `Error`, not `1.633e16`.** Floating point has no opinion about
asymptotes — `tan(90 * π/180)` is a large finite number, and on a panel it reads
as a real answer. The check is on the degrees the user typed, before any
conversion rounds them away. Degrees only, and that is not an omission: in
radians the argument is `π/2`, which cannot be typed exactly, so there is no
exact case to catch.

**The angle mode survives `C`.** It is a setting the `DEG`/`RAD` key displays,
not part of the sum being worked. Clearing the calculation should not silently
move the calculator to a different trigonometry.

**Where the eight keys came from: the register column.** Replacing the twelve
STO/RCL buttons with tap-to-recall and hold-to-store narrows that panel from
428 px to 240 px, and the freed width buys the keypad two more columns at
**112 px** — within a pixel of the 113 px keys it had before. Nothing shrank to
make room; the keypad grew into space the register chrome had been spending.
The three zones now tile the width exactly: `12 + 540 | 12 | 240 | 12 | 1092 +
12`.

**Six registers, not eight** — #509 posed this as "6 vs 8", and the answer is a
measurement rather than a preference. The row is the touch target now, and six
rows in the 424 px panel are 60 px (7.8 mm) tall. Eight would be 45 px — 5.9 mm
— which is too short to hold reliably, and **a hold that misses silently
recalls instead of storing**, which is the worst possible failure for this
gesture. Six it is.

**Register persistence is `kdeskdash:calc:regs` on the control Redis**, written
on every store and read on the way into the mode. The contract is pure and
host-tested (`calc_regs_serialize` / `calc_regs_parse`); the mode does only the
I/O. Two things worth keeping:

- **`%.17g`, not the display's `%.12g`.** `%.17g` round-trips a double exactly.
  Anything shorter would shorten a stored `sqrt(2)` a little more on every
  restart — a slow corruption that looks like nothing at all.
- **The parse is all-or-nothing.** A truncated or hand-edited line is rejected
  whole and leaves the register file untouched, rather than restoring the
  registers that happened to come before the bad token. Same rule
  `kvscf_parse_launcher` follows for the launcher layout, and for the same
  reason: a half-restored state is worse than no restore.

The read is attempted on `activate`, not in `_create` — modes are built before
the control Redis is dialled, and the endpoint is optional anyway. A `regs_loaded`
latch makes the read retry on a later activate if it did not land, and a local
store sets the latch too, so a stale remote line can never overwrite work the
user did while Redis was down.

### The LVGL trap this turned up

**Tap and hold on one widget needs `LV_EVENT_SHORT_CLICKED`, not
`LV_EVENT_CLICKED`.** LVGL sends `CLICKED` on release *whether or not* a long
press already fired (`lv_indev.c`, the release path) — so a `CLICKED` handler
for "tap" would recall over the value the hold had just stored, and every store
would look like a no-op. `SHORT_CLICKED` is the one gated on `long_pr_sent == 0`,
which is exactly the distinction, so no manual suppression flag is needed.

The swipe guard also works on `LONG_PRESSED`, and that was checked rather than
assumed: `indev_gesture()` runs earlier in the same press iteration than the
long-press timer check, so a swipe that lingers over a register row is already a
gesture by the time the hold could fire.

Written up in `docs/solutions/best-practices/lvgl-tap-and-hold-on-one-widget.md`.

## WI #1136 — the clock mode

### What shipped

`src/modes/clock.c` is now thin placement over shared pieces, in three panels
that tile the 1920 px width exactly (`12 + 620 | 12 | 640 | 12 | 612 + 12`):

- **left** — `kd_clock_widget`, the *same* widget the Launcher's side pane
  renders. The pane is 620×424, comfortably over `clock_core`'s large-tier
  threshold of 520×380, so the tier falls out of the arithmetic rather than
  being asserted here.
- **middle** — the almanac: the long date, the ISO week and the day of the
  year, then either the configured world clocks or three elapsed bars.
- **right** — the stopwatch, unchanged in behaviour and fixed in layout.

The mode no longer formats a single time itself. Everything about how a clock
face looks lives in `clock_widget.c`, which is what #1136 was for: the panel
had two clock implementations that could disagree, and now has one.

### The "fill the space" question, and what I did about it

#1136's details say the extra content is *"undecided and worth its own
brainstorm rather than being guessed at here"*, listing weather, date,
sunrise-sunset, extra timezones and the stopwatch as candidates. That is Ken's
call. Parking the sprint over it would have stranded #509 too, so the rule I
worked to — flagged on the proposal at the start, not in the wrap-up — was:
**fill the space only with what this repo can already compute, and leave a
declared seam for whatever Ken decides.**

So: the long date, the ISO week, the day of the year, and three "elapsed"
bars (day / week / year). All of it is arithmetic on the clock the mode is
already showing — no new feed, no new dependency, no new decision. **Weather
and sunrise/sunset were deliberately not built**: each needs something this
repo does not have (a feed, an ephemeris), which makes each a deliverable with
its own decisions rather than a rendering choice.

The seam is `KDESKDASH_CLOCK_ZONES` — `"Asia/Tokyo,HQ=Europe/London"`, up to
four faces, parsed in the pure core, every malformed entry skipped so a typo
costs one row rather than the mode. It defaults to empty.

**Configured content wins the lower two thirds.** When zones are set the panel
shows them; when they are not, it shows the elapsed bars. Both fit that space
and showing both does not — and a device that has named its zones should never
be showing derived filler *instead* of them.

### Decisions

**Progress percentages truncate, and come from the local wall clock.** A bar
that sits at 100% for the last half hour of the day is lying, so the range is
0..99 and a unit reads 99 until it actually ends. And on the spring-forward
day the local day is 23 real hours, but local noon still reads 50%: a user
glancing at a clock panel reads the wall, and "Day 48%" beside a 12:00 would
look broken. Both properties are asserted in `test_clock_core.c`, along with a
sweep over a year of samples that every percentage stays in range.

**`kd_clock_zone_face` restores the process timezone**, because the local face
depends on TZ staying pinned where `kd_clock_widget_create` put it. Two traps
in eight lines: `getenv` returns a pointer *into* the environment that the
following `setenv` may invalidate, so the old value is copied out before rather
than dereferenced after; and an absent TZ is restored as absent rather than as
the zone we borrowed. Both are tested.

**An unknown zone name is not an error** anywhere in the C library — glibc
silently falls back to UTC. So a mistyped zone shows UTC's time under whatever
label the config gave it. That is documented in the header and the README
rather than defended against, because there is no honest way to tell a bad zone
from `Etc/UTC` without shipping a zone list.

**The zone faces are minute-gated, the date second-gated, the stopwatch every
tick.** The main loop runs at ~100 Hz and almost nothing here changes that
fast. The zones matter most: each face costs a `setenv` plus two `tzset` calls,
which is file I/O on the UI thread, and they display `HH:MM`. Four zones now
cost eight `tzset` calls a *minute* rather than eight hundred a second.

**The stopwatch buttons gained the swipe guard** they never had. Every other
`CLICKED` handler in this repo carries it; clock mode's two were written before
the rule and were quietly missing it, so a swipe released over Start toggled
the stopwatch instead of changing mode.

### The digit-jitter fix, and the wrinkle it turned up

`docs/solutions/best-practices/proportional-digits-move-their-neighbours.md`
named clock mode's stopwatch as still carrying the bug at 10 Hz, deferred in
sprint 027 as out of scope with the note that #1136 would rebuild the mode
anyway. This is that rebuild, so it is fixed: the readout is left-aligned
inside a box pinned to its widest rendering, and the caption above and buttons
below no longer move.

The wrinkle, now folded back into that doc: **"the widest rendering" is not one
string** when the readout's *length* changes and not just its glyphs. `0:00.0`
and `12:34.5` differ by a whole digit — pinning to a fixed `M:SS.s` clips after
ten minutes, and pinning to a generous maximum leaves the readout visibly
off-centre for the first nine. So the width is recomputed when the
minute-digit count changes: twice an hour, not ten times a second.

## Verified on rpidash2

Pushed with `just push-dev` from **kai** (the host that runs the deploy — a
reachability check from anywhere else would have measured the wrong thing), and
the board was **fully restored afterwards**: back to the published
`0.27.0-31a285b`, `kdeskdash:active_mode` back to `claude`, the seeded
`kdeskdash:calc:regs` key deleted, 0 restarts, and neither committed host env
file touched.

What the panel actually showed:

- **Calc** — the 9×4 keypad with all eight new keys in place, the register
  column with its `tap RCL / hold STO` hint, and the keys measurably the size
  the arithmetic predicted.
- **Register restore, end to end.** Seeding `kdeskdash:calc:regs` with
  `0:1.4142135623730951 3:-1.5 5:0` and entering the mode showed R0 `1.41421`,
  R3 `-1.5`, R5 a bright `0`, and R1/R2/R4 muted `---` — including the subtle
  case that a register holding **zero** must read as *set*, not as empty.
- **Clock** — local `00:22` PDT against UTC `07:22`, `Friday 11 September
  2026`, `Week 37   Day 254 of 365`, and the bars at Day 1% / Week 57% /
  Year 69%. All three check out by hand for 00:22 on a Friday that is day 254:
  22 min of 1440 is 1%, ISO-weekday 4 of 7 plus 22 min is 57%, 253 days of 365
  plus 22 min is 69%.

**The first clock build did not survive this.** It rendered correctly and was
three-quarters empty — the long date and week line crammed at the top of a
640×424 panel with nothing beneath them, which is precisely the failure #1136
exists to fix. The elapsed bars were the answer to what the screenshot showed,
not something planned from the desk.

## Not verified, and worth saying

**The world-clock rows have never been on a panel.** Neither board sets
`KDESKDASH_CLOCK_ZONES`, so the default path is the one the fleet runs and the
one screenshotted. The parsing is host-tested hard (eleven assertions covering
labels, spacing, malformed halves, over-long names, the cap and the degenerate
arguments), and the rendering is plain aligned labels with no grid — so the
segfault shape from
`lvgl-grid-children-need-cells-immediately.md` does not apply. But confirming
it visually would have meant editing a live device's env file and restarting
the service from an unattended leg, which is a worse trade than saying this
plainly.

## Follow-ups

- What actually goes in the clock's spare space is still Ken's to decide. The
  elapsed bars are an honest default, not an answer to the brainstorm #1136
  asked for; weather and sunrise/sunset remain unbuilt and each wants its own
  item.
- The calculator persists its registers but not its angle mode. That was scope
  discipline — #509 says "register persistence" — and the `DEG`/`RAD` key names
  its own state on screen, so a surprise is immediately readable in a way a lost
  register is not.
