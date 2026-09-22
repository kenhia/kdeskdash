# Sprint 042 — Lowercase the card key; make the Code list ready for korg's projects

**Proposal:** korg:3047 (slice of program korg:3062, "Low-hanging fruit, run 2").
**Covers:** WI 2277 (the `rpiDash2` card key), WI 2928 (closed non-favorite rows
in the Code list).
**Branch:** `042-card-key-and-project-rows`, off `95e4f93`.

Overseen sprint: run as karc leg `kdeskdash-2feb5b` on kai, reviewed on the
proposal thread, shipped on the overseer's green light.

## Goal

Two small panel items that turn out to be opposites. One is a change with a
mandatory ops step attached. The other is mostly a *measurement*, where the
honest answer to the headline question is "nothing to change" — and where the
work that does exist is in the half of the item nobody was looking at.

## Premise check

Both held.

- **2277** — `segment_ok()` in `src/service_card.c` permits `A-Z`, and
  `service_card_short_host()` truncates at the first `.` and copies the label
  through verbatim. rpidash2's own hostname is `rpiDash2`, so the key published
  is `kpidash:services:deskdash:rpiDash2`, exactly as the item says.
- **2928** — `running` and `favorite` are two independent bools on the wire
  (`kvscf_feed.h`), both parsed independently (`kvscf_feed.c`). The renderer is
  `src/modes/foreground.c`.

## WI 2277 — lowercase the host segment

One line of intent, in the one place the host segment is derived.
`service_card_short_host()` now lowercases the label it returns.

Two decisions inside that:

- **Normalise, don't reject.** `segment_ok()` still permits `A-Z`, so
  `rpiDash2` remains a *valid* hostname that gets canonicalised — it does not
  degrade to the `_` sentinel. A real hostname is not an error.
- **In `short_host`, not in `service_card_key`.** `service_pub.c` derives the
  host once and feeds the same string to both the key and the payload's `host`
  field, so normalising at the derivation point is what keeps those two from
  ever disagreeing. Doing it in `service_card_key` would fix the key and leave
  the payload saying `rpiDash2`.

Tests pin mixed case, all-caps, mixed-case FQDN (truncate *then* lowercase), and
that digits/`-`/`_` survive untouched.

### The ops step this change cannot skip

Service cards have **no TTL**. The moment rpidash2 runs this build it publishes
`deskdash:rpidash2`, and the old `deskdash:rpiDash2` card stays on the board
forever — going red after 60 s, because nothing ever removes it.

So the deploy has a mandatory second step, and it must run **after** the new
version is live on rpidash2 (prune first and the old binary just republishes the
stale key within 15 s):

```sh
/home/ken/src/tools/kpidash/scripts/kpidash-cards prune --service deskdash:rpiDash2
```

Verified present on kai this sprint. Per kpidash's `docs/CLIENT-PROTOCOL.md` §9
a prune is two operations — `DEL` the data key *and* publish the evict command —
which is exactly why the helper exists and why this is not a hand-rolled
`redis-cli DEL`.

## WI 2928 — the measurement, and what it actually found

### Item 1 — the renderer: no change needed

`fill_cell()` (`src/modes/foreground.c`) draws a closed row from `running`
**alone**:

- label colour — `in->running ? app_color : COLOR_MUTED`
- host colour — dimmed on `!in->running`
- marker — `○` on every `!running` row; `★` only on `running && favorite`
- tap — `relaunch = !st->items[idx].running`, with `favorite` never consulted

That is exactly wire-contract §5's suggested rendering, and exactly the call the
proposal's notes make. A `running:false, favorite:false` project row already
renders faithfully as a dimmed `○` row and already publishes the same focus
command carrying the folder URI. **The panel is ready as it stands**, and the
honest outcome of item 1 is that there was nothing to write.

Tests were added anyway: the shape had never once been exercised. `running` and
`favorite` are independent on the wire but nothing had ever *sent* the
false/false combination, so nothing pinned it.

### Item 2 — the look: already correct, and deliberately not touched

Per the proposal's ruling, no new visual treatment was invented for a closed
non-favorite. It gets the `○`, same as a closed favorite. kctrldeck WI 2927 is
Ken's open UI-refinement question and this sprint stays out of it.

### Item 3 — density: this is where the work was

Two real findings, and the item's own question is answered by the first.

**The panel re-sorts, so the publisher's array order does not survive.**
`kvscf_sort_by_label()` discards the feed order and sorts running-block-first,
then alphabetically by label. The deck orders its rows "open windows, then
favorites + korg-starred projects, then the rest alphabetically" — none of which
reaches the screen. So the item's question ("do the panels need an additive
`starred` field, or can they rely on array order?") answers: **neither.** Array
order cannot be relied on, and no new field is needed, because `favorite` is
already on the wire and already parsed.

`cmp_label` now sorts favorites ahead of non-favorites **inside the closed block
only**. Two reasons for that scoping:

- Today every closed row *is* a favorite, so the change is a provable no-op
  until the deck starts publishing project rows, and correct the moment it does.
- Applying it to the running block would reorder the open windows Ken looks at
  now. That is a behaviour change someone depends on, not a repair.

**The 64-row cap was inside the working range** — see *Repaired in passing*
below, which is where the overseer ruled this belongs.

Paging itself needed nothing: `KV_PER_PAGE` is 28, so ~40 rows is two pages and
the existing page nav and `N · p/pages` counter already handle it.

## What this sprint did not do

**It did not touch kctrldeck.** The deck publishing project rows is WI 2929,
which `depends_on` 2928 and is not in this run.

One decision is handed to it, recorded as a comment on 2929 rather than a new
item: the panel now sorts the closed block on `favorite`, so the deck must
decide whether a **korg-starred** project carries `favorite:true` on the wire —
in which case nothing more is needed — or whether "Ken's VS Code favorite" and
"starred in korg" must stay distinct there, in which case an additive `starred`
field is the change, and it is kctrldeck's contract to amend. That is another
repo's contract, so it is not this sprint's to decide.

## Repaired in passing

**`KV_INSTANCES_MAX` 64 → 128 — a silent, wire-ordered truncation of the Code
list.** Filed here on the overseer's ruling (proposal korg:3047, clearance
comment): the evidence removed the decision, so it is a repair rather than a
feature of WI 2928.

*What was broken.* The cap was sized when the Code list was open windows plus a
handful of favorites. ~40 project rows arrive *before a single open window*, so
64 had stopped being headroom. Worse than an ordinary cap:
`kvscf_parse_append()` fills in **wire order** and stops dead at the cap, while
`kvscf_sort_by_label()` runs *afterwards*. An overflow therefore does not drop
the least important rows — it drops whichever the publisher happened to list
last, **which can be open windows**, with no signal anywhere on the panel.

*What was done.* Raised to 128, so ~40 projects, every favorite and a full set
of open windows fit inside the array. Cost ~1.1 KB per index across
`foreground.c`'s three arrays (measured: instance 576 B, edge 396 B, appitem
168 B) — ~146 KB total, `calloc`'d, not on the stack.

*Which gate proves it.* `just check` — `test_kvscf_feed` gained
`test_capacity_holds_a_full_project_list`, which parses an 80-row feed and
asserts all 80 land (the old cap would have truncated to 64). The existing
`test_cap` still pins that the parser clamps rather than overflowing.

*What was deliberately not done.* A cap overflow still shows no signal on the
panel. That needs a decision about what to render, and the overseer declined it
as speculative now that worst case (~60–70 rows) sits well under 128. **The
condition that reopens it:** if the deck ever publishes enough project rows to
approach 128, the signal question becomes real and gets filed then, with the
render decision named.

## Verification

- `just check` — 24/24 tests pass, including the four new assertions.
- `cmake --build build-pi --target kdeskdash` — the aarch64 cross build compiles
  and links with the raised cap and the changed header. Run because this sprint
  edits a header the Pi binary uses and grows a mode's state; `just check` is the
  host gate and would not have caught a cross-build break.
- No sysroot refresh was needed (`~/pi-sysroot` is absent on kai; the existing
  `build-pi` tree configures and builds as it stands).
