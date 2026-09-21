# ABS shrinkage notes — main case body

**Working notes, not published guidance.** These STLs are still PLA-dimensioned;
see the ABS section of [`docs/hardware.md`](../docs/hardware.md) for the two
reasons they are not ABS-ready. This file records the shrinkage half of that
problem and the settings being trialled. The **blind overhang** is a separate,
still-open model change and is not addressed here.

Last updated 2026-09-21.

**Which field the 100.545 % went into — answered by Ken, 2026-09-11.** It was a
**uniform scale of the model in the slicer**, i.e. the per-object scale box, not
the filament Shrinkage field. That is the right mechanism (see
[what Shrinkage actually does](#what-bambu-studios-shrinkage-setting-actually-does)),
so the settings below stand as written. **The dimensional result was not
recorded** — there is no post-print measurement of the compensated body, so
nothing here claims the 277 mm came out right. ABS remains **not recommended**
for the models in this repo; the blind overhang is unchanged and is a model
problem, not a settings one.

## Measurements

Both from the first trial prints — Bambu H2D, Bambu ABS Black, `0.20mm Standard
@BBL H2D` base profile, against the in-service PLA case.

### Z — the 277 mm length

The body prints standing on its right end, so the long dimension is build height.
That matters; see [the Bambu Studio section](#what-bambu-studios-shrinkage-setting-actually-does).

| | |
|---|---|
| Model length (`deskdash_case.stl`) | 277.0 mm |
| PLA | 277 — correct |
| ABS | 275.5 mm — short by 1.5 mm |
| ABS shrink | 1.5 / 277 = **0.5415 %** |
| Compensation scale | 277 / 275.5 = **100.545 %** |
| Compensated model length | **278.508 mm** |

### XY — the 120 mm cross-section height

| | Measured | vs Fusion nominal |
|---|---|---|
| Fusion 360 nominal | 120.344 | — |
| PLA | 120.52 | **+0.146 %** |
| ABS | 119.75 | **−0.494 %** |

ABS against the PLA part rather than against CAD: 119.75 / 120.52 = **−0.639 %**.

So there are two defensible XY targets, and **they bracket the Z number**:

| Target | Scale |
|---|---|
| Fusion nominal (120.344) | 100.496 % |
| The PLA part (120.52) | 100.643 % |
| *(Z, for comparison)* | *100.545 %* |

**Note this contradicts the usual guidance.** Published ABS figures put XY shrink
at 0.7–0.8 % and above Z, on the reasoning that Z is mechanically pinned by
nozzle positioning while XY contracts layer by layer. Measured here, XY against
CAD (0.494 %) is *smaller* than Z (0.5415 %). Trust the part, not the handbook.

### The fixed offset — confirmed, and asymmetric

A second measurement, interior front wall to rear wall (also XY given the print
orientation), taken to test whether the deviation is a fixed mm offset rather
than a percentage:

| | Fusion nominal | PLA | ABS |
|---|---|---|---|
| Exterior "height" | 120.344 | 120.52 (**+0.176**) | 119.75 (**−0.594**) |
| Interior front→rear | 93.065 | 92.70 (**−0.365**) | 92.42 (**−0.645**) |

**Confirmed, and worse than inferred from the exterior alone.** PLA runs
+0.088 mm/side on outside surfaces but **−0.183 mm/side on inside** ones — walls
print ~0.27 mm thicker than drawn, and the error lands mostly on the inside.
That asymmetry is expected: the slicer anchors the outer surface and lets the
inner one absorb the slop.

This is why no scale factor fixes the slot — a fixed mm offset does not respond
to a percentage. 0.545 % of a ~6 mm slot is 0.033 mm, a sixth of a layer.

### ⚠ The two dimensions disagree about ABS shrink

| Reference | ABS vs PLA |
|---|---|
| Exterior 120 mm | 119.75 / 120.52 = **−0.639 %** |
| Interior 93 mm | 92.42 / 92.70 = **−0.302 %** |

~0.31 mm unaccounted for. Working backwards from the exterior shrink, the ABS
print's interior surfaces sit 0.026 mm/side off nominal, against 0.183 mm/side
for PLA — a factor of seven on the exact quantity that sets slot width.

Unresolved. Candidates: caliper inside-jaws across a 93 mm cavity is ±0.1–0.2 mm
work; a 277 mm ABS shell can bow 0.2 mm; and the ABS profile runs 4 wall loops
against PLA's stock 2, which can move where the inner boundary lands.

**Do not let the interior number move the scale factor** — exterior measurements
on an accessible edge are the trustworthy ones. It does mean the hole
compensation value below is not yet knowable from these two parts.

## Settings

### Scale — uniform 100.545 %, all three parts

Uniform, not per-axis: it lands XY at 120.40 (0.06 over CAD, 0.12 under PLA),
which sits between the two targets above. Per-axis would split that more finely,
but 0.1 % on a 120 mm part is 0.12 mm — about the real repeatability of caliper
work on a printed edge.

**Scale all three STLs by the same factor**, not just the body. The end caps mate
to it through features spanning ~60 mm; 0.545 % of that is 0.33 mm, which is real
against M3 clearance holes. Scaling the body alone would introduce a
misalignment that does not exist today.

Baking compensation into the CAD is the cleaner long-term fix and is what the
published STLs will eventually carry — but it forks the model per material, so
slicer-side scale is the better trial mechanism.

### Enable Precise Z height

Print Settings → Quality → **Precise Z height**, Advanced mode, flagged
experimental in its tooltip. 277 × 1.00545 = 278.508 mm = 1392.54 layers at
0.20 mm — not reachable. Without it the slicer rounds:

| Layers | Sliced height | After 0.5415 % shrink | Error |
|---|---|---|---|
| 1392 | 278.4 mm | 276.89 mm | −0.11 mm |
| 1393 | 278.6 mm | 277.09 mm | +0.09 mm |

With it, the last few layers absorb the remainder. (Alternative if it misbehaves:
absorb it in the first layer — 0.30 mm + 1391 × 0.20 mm = 278.50 mm exactly.)

### The slot — a fixed offset, not a percentage

Print Settings → Quality → **X-Y hole compensation**, mm, Advanced mode.
Tooltip: *"Holes of object will be grown or shrunk in XY plane by the configured
value. Positive value makes holes bigger... used to adjust size slightly when the
object has assembling issue."*

**It is a per-side offset, so a slot gains 2× the value in width.**
`PrintObject::_shrink_contour_holes()` applies it as a single Clipper polygon
offset (`offset(hole, -hole_delta)`; hole polygons are stored clockwise, which is
why positive values grow them). Not a diameter or width delta — easy to
double-correct if you assume otherwise.

**Value: not yet determined** — see the disagreement above. The PLA offset argues
for +0.18 mm/side; the ABS part's own numbers argue for closer to +0.03. Settle
it with a coupon rather than guessing at 12 h per attempt:

> **Cut a test coupon.** Bambu Studio's cut tool, ~20 mm slab off the body, same
> standing orientation so the cross-section, slot and an insert boss print
> exactly as they would for real. 30–45 min in ABS. Settles the offset, the
> compensation value, and whether the inserts still grip. It will *not* validate
> the Z scale — that needs full height — but Z is the number we are confident in.

If skipping the coupon, **+0.10 mm** is the safer hedge: still +0.20 mm of slot
width, on the side of the uncertainty that does not loosen six insert bores
carrying the whole assembly.

It widens **every** internal contour, inserts included. And if the sliced preview
treats the groove as an open-sided contour rather than a hole, the equivalent
knob is **X-Y contour compensation**, same units and same per-side semantics.

**Measuring tools.** For the slot, **feeler gauges** (~$10) beat anything
obtainable from the 93 mm span — stack them in the PLA and ABS slots for real
widths to ±0.02 mm. For the 4.7 mm insert bores, drill bit shanks from a metric
index work as go/no-go gauges.

### Leave the filament Shrinkage field at 100

Everything above is per-object scale and mm offsets. Do not also set the filament
Shrinkage field — it would double-compensate XY, and its semantics are inverted
relative to a scale box anyway (see below).

## What Bambu Studio's "Shrinkage" setting actually does

Verified against the Bambu Studio source (`master`, checked 2026-08-01), because
the secondary write-ups on this are misleading.

**Location:** Filament Settings → **Filament** page → **Basic information**
group → **Shrinkage**, in `%`. It is an *Advanced*-mode parameter
(`mode = comAdvanced`), so it is hidden until the parameter mode selector is set
to Advanced or higher. To get there: click the **`···`** / edit icon next to the
filament, which opens Filament Settings.

Three things about it that are easy to get wrong:

**1. It is XY-only. It does not touch Z, and therefore cannot fix the 1.5 mm.**
The compensation is applied to the 2D slice contours, layer by layer —
`PrintObjectSlice.cpp` scales each layer's polygons by `1/(filament_shrink/100)`
and never adjusts layer count or layer height. The layer count and final layer
height are identical before and after changing it. This is a known and still-open
gap: BambuStudio issues
[#5699](https://github.com/bambulab/BambuStudio/issues/5699) (Jan 2025),
[#7765](https://github.com/bambulab/BambuStudio/issues/7765) (Aug 2025) and
[#9153](https://github.com/bambulab/BambuStudio/issues/9153) (Dec 2025) all ask
for Z compensation and all remain open as of Bambu Studio 2.7.1 / 2.8.1 beta.
There is no `shrinkage_compensation_z` in the codebase; `filament_shrink` is the
only shrinkage key that exists.

**2. The number you type is the *shrink*, not the *scale*.** The tooltip is
explicit: *"Enter the shrinkage percentage that the filament will get after
cooling (94% if you measure 94mm instead of 100mm)."* The slicer applies the
reciprocal. So the value for this filament would be **99.46 %**, not 100.545 % —
entering 100.545 makes parts 0.54 % *smaller*, the wrong way.

**3. Only the perimeter filament's value is used** (`wall_filament`). Irrelevant
for a single-material print, but worth knowing.

## Still to verify

- [ ] **Resolve the exterior/interior shrink disagreement** — the open question
      blocking a hole-compensation value. Coupon print, or feeler-gauge the slot
      on both existing parts.
- [x] **Which field did 100.545 % go into?** Answered 2026-09-11: the uniform
      per-object scale in the slicer, not the filament Shrinkage field. The
      mechanism is correct; see the note at the top.
- [ ] Does uniform 100.545 % land the body at 277 and the cross-section near
      120.4? **Still open — no dimensional result was ever recorded.** Whether
      a compensated body was printed at all is not recorded here either; what
      is certain is that no post-print measurement of one exists. The
      compensation is unvalidated in both directions, so measure before anyone
      treats 100.545 % as proven.
- [ ] Does `precise_z_height` behave on a ~1393-layer part?
- [ ] What X-Y hole compensation gives a slot that accepts the panel without
      force *and* insert bores that still grip?
- [ ] Whether one data point per axis is enough. ±0.05 mm of caliper error is
      the same order as the differences being tuned between candidate scale
      factors — and the interior measurement is worse than that.

## Sources

- BambuStudio `src/libslic3r/PrintConfig.cpp` — `filament_shrink`,
  `precise_z_height`, `xy_hole_compensation`, `xy_contour_compensation`
  definitions, labels, tooltips, modes, defaults
- BambuStudio `src/libslic3r/PrintObjectSlice.cpp` — the XY-only slice-contour
  scaling (`scale = 1 / (filament_shrink / 100)`)
- BambuStudio `src/slic3r/GUI/Tab.cpp` — Filament page, Basic information group
- BambuStudio issues [#5699](https://github.com/bambulab/BambuStudio/issues/5699),
  [#7765](https://github.com/bambulab/BambuStudio/issues/7765),
  [#9153](https://github.com/bambulab/BambuStudio/issues/9153)
- [Bambu Lab wiki — 3D prints shrinkage](https://wiki.bambulab.com/en/knowledge-sharing/3d-prints-shrinkage)
  (not directly readable at the time of writing — HTTP 402 — so everything above
  is from the source tree and the issue tracker instead)
