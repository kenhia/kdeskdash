# 022 — Assembly photos, and ABS demoted from "recommended" to "not yet"

WI #759 · docs-only · follow-on to [021](021-hardware-guide.md)

## Goal

Sprint 021 shipped the build guide with a visible IOU: *"Photos are coming …
when the case is reprinted in ABS."* The ABS reprint happened. It produced the
photos — and also produced two reasons not to recommend ABS yet. Both land here.

## What shipped

- **[docs/assembly.md](../docs/assembly.md)** — the step-by-step build, 13
  photos, moved out of `hardware.md` rather than duplicated there.
- **`docs/images/assembly/`** — the photos, resized and renamed.
- **[docs/hardware.md](../docs/hardware.md)** — ABS section rewritten; the
  "photos are coming" callout removed; assembly section replaced with a pointer.
- **README** — build-guide pointer now names both docs; layout tree updated.

## Decisions

**The assembly walkthrough moved; it was not copied.** `hardware.md` promised
"what to buy, what to print, and how it goes together" and delivered all three.
Adding a photo-driven `assembly.md` alongside it would have meant two step lists
drifting apart on the next revision. So `hardware.md` is now buy-and-print, and
"how it goes together" is a page of its own with the photos inline. The prose
that survived is the 021 prose — this sprint added captions and the reasons
behind two steps, and did not rewrite what was already right.

**Anchors moved with it.** The BOM's optional-VHB row links into the tape
discussion, which now lives in `assembly.md`; both of its links were repointed
rather than left to 404 silently. Every cross-doc link and image path in both
files was checked mechanically, not by eye.

**Step order follows the photos, and the photos disagreed with the doc.** 021
listed "attach the left side" as step 5, after mounting the Pi and fitting the
adapter. The photos show the end cap going on before the monitor sub-assembly
comes anywhere near the case — which is the better order, because it gives you a
case that stands up on its own and one open end to work through. Renumbered to
match. Separately, 021's steps 6 and 7 ("route the cables", "connect them") are
one continuous operation in the photos and merged into one step.

**Cables are threaded through the case *before* they are connected.** Worth
recording because the obvious guess is wrong and the photos are easy to
misread. The first draft of this doc had the cables plugged in on the bench and
fed in with the monitor; 021's text said otherwise, and IMG_2826 backs 021 up —
the cables come up out of the case to a monitor that is still lying outside it.
Cables first means never hunting for a port by feel inside a closed case.

**The photos say which print they are.** The set was shot on the *failed* ABS
body — the shrunk one — except the last three, which are the PLA case the
dashboard actually lives in, because the desk needed its dashboard back. The
surface finish visibly changes between photo 10 and photo 11, so the doc says so
outright rather than letting a reader wonder whether they are looking at two
different builds. A note that the pictures are not all the same physical part
costs one paragraph and buys the rest of the guide its credibility.

**Photos: 1600 px long edge, quality 82.** 11.2 MB of originals became 3.1 MB.
The originals are 2880×2160 phone shots; GitHub renders README/doc images at
about 800 px CSS width, so 1600 is the retina-crisp size and anything above it
is bytes every future `git clone` pays for and no reader sees. Renamed
`NN-what-it-is.jpg` — `IMG_2831.jpg` tells a reader nothing, and the numeric
prefix keeps them in assembly order in a directory listing.

**ABS is now explicitly *not* recommended, with the reasons.** 021's guide read
as though ABS were the intended upgrade and gave a settings table for it. That
was true of the intent and false of the result:

- **Shrinkage.** Over the 277 mm body, ABS shrink put the first print ~1.5 mm
  short. That is a model change — measure, compute the compensation, regenerate
  the STL, reprint, test-fit — so there is no slicer setting a reader could use
  to work around it. Printing the published STL in ABS today burns 12 hours to
  rediscover this.
- **Blind overhang.** ABS sagged where PLA bridges fine. It came out usable, but
  the intended fix is an access hole in the back that lets the overhang be
  closed up properly — again a model change, and again not something settings
  reach.

The settings table stays, retitled and reframed as *what the trial print used*
rather than *what you should do*. The heading now recommends PLA outright: PLA
is what is in service, and the enclosure heat that motivated the ABS experiment
has not in practice turned out to be a problem.

## Verification

Mechanical: all 13 image paths resolve, no orphan files in
`docs/images/assembly/`, and every cross-document link and anchor
(`hardware.md` ↔ `assembly.md` ↔ README) was checked to exist. `just check`
passes — this is a docs-only change and touches no source.

Editorial: the captions are a first pass written from the photos. The shooting
order was given as authoritative and is preserved exactly; what each photo
*shows* is inference and gets corrected on review.

## Follow-ups

- **Compensated ABS STLs.** Blocked on the measure/regen/reprint/test-fit loop.
  When they land, `hardware.md`'s "ABS: not yet" section becomes an ABS
  recommendation and the settings table is promoted back.
- **Access hole for the blind overhang.** Ships with the same model revision.
- **Smaller-printer STLs.** Still open from 021 — the three-part split exists
  but wants indexing features.
