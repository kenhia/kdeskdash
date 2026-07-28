# 021 — Hardware: BOM, assembly guide, and the case STLs

korg:715 (proposal) · WI #709 · docs-only

## Goal

Close a promise the README had been carrying since the hardware section was
written: *"3D Printed case (work in progress, will include STLs once I finish
the design)"*. The design is done, so this sprint commits the three STLs and
writes the build guide that makes them useful — BOM, print settings, assembly.

kdeskdash could tell you what it runs on but not how to build one. That gap is
what this closes.

## What shipped

- **[docs/hardware.md](../docs/hardware.md)** — the build guide.
- **`stl/`** — `deskdash_case.stl` plus the two end caps.
- **README** — the work-in-progress line replaced with a pointer to the guide,
  and `stl/` + `docs/hardware.md` added to the Project layout tree.
- **[Issue #25](https://github.com/kenhia/kdeskdash/issues/25)** — a standing
  place for WiFi reliability reports.

## Decisions

**`stl/` at the repo root, guide in `docs/`.** The STLs are downloadable
artifacts, not prose; a top-level directory named for exactly what it holds is
the most discoverable thing on GitHub. `hardware/` would only earn its nesting
once there is source CAD or BOM photography to group alongside. The guide itself
goes in `docs/` per the repo's existing convention.

**The README's hardware table stays where it is.** It looks like duplication but
is not: the table is *target* information for someone deploying (which board,
which DRM node, which evdev path), while the new doc is *build* information for
someone assembling. Different readers arriving with different questions, so they
should not be merged.

**The WI's ten steps were rewritten, not pasted.** They were working notes. The
details worth preserving are the ones that cost real money or time to learn, and
they needed to be *reasons* rather than asides — why the inserts have to cool
(they spin, and a spun insert is hard to recover), why "carefully" is in
"carefully remove the PCB", why PLA/PETG want retightening twice.

**ABS settings: more walls, same infill.** The guide originally said ABS at 40%
gyroid was the intended upgrade. On actually starting that reprint, the reasoning
changed and the doc now says why. The PLA case was already strong enough in bulk
and ABS at identical parameters is stronger still, so raising infill would mostly
have added hours to a 12-hour print. What *did* go up — wall loops 4, bottom
layers 5 — is aimed at the one place this part sees real load: the six heat-set
inserts the end caps and the whole assembly hang off. More walls put plastic
exactly there; more infill would not. Recorded as a settings table with a *why*
column, plus a plate-layout render, since "what changed from the stock profile
and for what reason" is the part a reader can act on.

**A real VHB link, found mid-sprint.** The link in the WI (`B0FPQ3VWPB`) is 3M
*removable* heavy-duty mounting tape — not VHB. That is not a nitpick: the
failure being fixed is heat-driven adhesive creep, and a tape engineered to
release cleanly is the wrong tool six inches from a Pi under load. Replaced with
3M VHB 5952 (`B0016HM7SE`, 3/4" × 15 yd), whose width suits a PCB edge strip
without slitting it lengthwise. The guide now explains the distinction, and
notes the tradeoff — VHB is permanent, so dry-fit first.

## Verification

Every link in the finished doc was fetched, and not just for a status code:
Amazon returns 200 for captcha and error pages, so each product page's `<title>`
was checked against what the BOM claims it is. All six products matched. The two
Raspberry Pi entries are deliberately *search* links rather than product links,
since model listings churn faster than a doc can track.

Internal links (the three STLs, the README anchors) resolve.

## Follow-ups

- **Photos.** Deferred deliberately — a full assembly set will be shot during
  the ABS reprint. The guide carries a visible note so the gap is not mistaken
  for an oversight.
- **Smaller-printer STLs.** The main body is ~277 mm and needs a large-format
  printer. The three-part split exists but needs indexing features before it is
  worth publishing, so the guide invites an issue rather than promising a date.
- The tape step is still provisional in one respect: the Pi 5 unit's PCB came
  loose, the Pi 4's has not yet. If the Pi 4 also fails, the VHB step moves from
  optional to recommended.
