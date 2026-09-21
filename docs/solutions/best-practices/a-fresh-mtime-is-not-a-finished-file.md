---
title: A fresh mtime is not a finished file — publish by rename, not in place
date: 2026-09-20
category: docs/solutions/best-practices
problem_type: best_practice
module: bmp_write, screenshot, panel_state, scripts/kddss
component: io
applies_when:
  - One process writes a file and another reads it without coordination
  - The reader decides "it is ready" from mtime, existence, or a size threshold
  - The file is big enough, or the device slow enough, that the write takes a moment
  - Or the file is state you must not lose half of after a power cut
tags: [atomicity, rename, screenshots, state-file, sd-card, wi-2308]
---

# A fresh mtime is not a finished file — publish by rename, not in place

## Context

korg **WI 2308**, opened during the sprint-035 ship. `scripts/kddss` asked the
panel for a screenshot and `cat`ed the BMP as soon as its mtime went fresh.
Three shots of five came back truncated — PIL rejected them with
`image file is truncated (842 / 586 bytes not processed)` — and a retry a few
seconds later worked every time.

Nothing was racy in the usual sense. The panel wrote to
`/var/lib/kdeskdash/kdeskdash-shot.bmp` with `fopen(path, "wb")`, which
**truncates the file and sets a fresh mtime on the first write**. At 1920×440×3
the BMP is ~2.5 MB and a Pi's SD card takes a visible moment to absorb it. The
reader's test was true from the first byte onwards.

It survived three years of use because it usually *did* finish in time. That is
the tell: this is a bug whose visibility is proportional to the size of the
file and inversely proportional to the speed of the disk, so it appears when a
panel gets busier or an image gets bigger, long after the code was written.

## The rule

**A writer publishes a file by `rename()`ing a complete one into place.** Write
`<path>.tmp` in the **same directory**, `fflush`, `fsync`, `fclose`, then
`rename`. `rename(2)` within one filesystem is atomic, so the target only ever
names a whole file: a reader sees the old one or the new one and never a
prefix of the new one.

Three consequences worth stating, because each is a thing somebody later tries
to "simplify":

- **The temp file must be in the same directory**, not `/tmp`. Across
  filesystems `rename` fails and a fallback becomes a copy, which is the
  original bug wearing a hat.
- **`fsync` before the rename**, or the atomicity is only with respect to other
  readers and not with respect to a power cut. On an SD-card appliance that is
  not a theoretical distinction.
- **On failure, `unlink` the temp and leave the target alone.** A failed
  capture that replaces a good one with a truncated one is strictly worse than
  a failed capture that changes nothing.

## Fix it on the writer, not on every reader

The tempting fix is on the reader: poll until the size is stable across two
samples, or compare it to the BMP header's declared file size. It works, and it
is wrong to *stop* there, because it fixes one reader.

`kdeskdash` had two — `scripts/kddss`, and the `deploy-panels` skill's
`kddss deploy-$V` step, which is where the bug actually bit during a ship — and
any future consumer would have arrived without the guard. One `rename()` in the
writer fixed all of them and let `kddss` get *simpler*: it now just waits for
the mtime to change, because a changed mtime finally means what it looks like
it means.

Reader-side stability checks are for files somebody else's program writes.

## The same shape, elsewhere in this repo

Sprint 039 needed the discipline twice in one change, which is what promoted it
to a rule:

- `bmp_write_file_atomic()` — the screenshot fix above.
- `panel_state_save()` — the durable panel state file, for the other reason. A
  torn state file after a power cut is worse than no state file, and the
  whole-file rejection rule (`panel_state_parse`) only protects you from
  *reading* one; the rename is what stops you writing one.

Both are pure stdlib and both are host-tested for what the promise actually is:
the temp sibling is gone after a success, and a **failed** write leaves the
previous target byte-identical. That second assertion is the one that catches a
regression, because a rename that quietly became an in-place write still passes
every happy-path test.

## Signature

- A consumer reads a file intermittently truncated, and a retry always works.
- The truncation is at an arbitrary offset, not a clean record boundary.
- The producer logs success — because it *did* succeed, a moment later.
- Bigger files, slower storage or a busier writer make it more frequent; none
  of them make it appear or disappear cleanly, which is why it reads as flaky
  rather than broken.
