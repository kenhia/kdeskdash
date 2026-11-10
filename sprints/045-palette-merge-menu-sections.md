# Sprint 045 — Palette neutrals merged, and configurable Menu sections

**Proposal:** korg:4370 (a slice of program korg:4368, "Backlog drawdown, November 2026").
**Covers:** WI 2259 (merge the near-duplicate neutrals, as Ken ruled in comment 2815) and
WI 668 (configurable Menu sections: names, count 1–3, header colours).
**Branch:** `045-palette-merge-menu-sections`, off `8f0a47b`.

This is an overseen sprint. It ran as karc leg `kdeskdash-04b689` on kai, and the
overseer's review happens on the proposal thread.

## Premise check

- **2259 holds.** All three dropped names were still in the table. `UTC_FROST` and
  `FADED_DENIM` were referenced from 10 files, which matches the triage list.
  `DUSK_SILVER` turned out to be referenced by nothing at all. Its usage note
  ("clock: local seconds") had gone stale when sprint 036 rebuilt the clock, so
  dropping it repaints nothing.
- **668 holds.** `modeset.c` accepted exactly `fun`/`ops` (`section_name_known`
  against the roster), and `menu.c` had the marked seam in `header_text`. Its layout
  was hard-coded 3×3, and the header was hard-coded coral.

## WI 2259: the panel's look changed on purpose

I applied Ken's ruling exactly as he gave it. The established names are the ones
that went:

| survivor | absorbs |
|---|---|
| `PALE_STEEL` #C8D0E0 | `UTC_FROST` #CFE0F5, `DUSK_SILVER` #B9C6DB |
| `DORMANT_SLATE` #6B7280 | `FADED_DENIM` #525D73 |

`TRUE_BLACK`/`VOID`, `PITCH_SLATE`/`DEEP_SLATE` and `ASH_MIST`/`STEEL_MIST` stay as
they are, by the same ruling. The palette went from 43 names to 40. The usage notes
of the two survivors now describe their wider use.

**This is a visible change, not a cleanup.** The screens it repaints:

- **Clock (mode and the Launcher's side pane).** The UTC time is a touch greyer
  (`PALE_STEEL`). The "UTC" caption, the UTC date and the clock mode's muted
  captions are lighter (`DORMANT_SLATE` is brighter than the old `FADED_DENIM`).
- **Dev and its graph.** Host names are `PALE_STEEL`.
- **Launcher.** The status line ("no token … view only") and the banner text are
  `DORMANT_SLATE`.
- **Muted text in calc, claude, foreground (Remote) and icons.** Each one's
  `COLOR_MUTED` alias is now `DORMANT_SLATE`.

I checked it on rpidash2 with before/after screenshots (`kddss`) of the menu, palette,
clock, dev and launcher, taken before and after a `push-dev` of this branch.
The pixel diff matches the list above: clock, dev and launcher change in their
text regions, palette only in its page count (the table lost three cards), and
the menu not at all.

## WI 668: configurable Menu sections

The grammar is the one the item sketched. Each clause is `name[#PALETTE]:id,id,…`,
and clauses are separated by `;`:

```
KDESKDASH_MODES="Play#EDGE_TEAL:game_of_life,golz,icons;Work#INSIDER_MINT:claude,launcher,foreground,clock,dev,calc;Etc:palette"
```

- **Names.** Any non-empty label up to 23 bytes, trimmed. `section_name_known` is
  gone. The roster's `fun`/`ops` are now just the default grouping. The Menu still
  capitalises the first letter, which is how legacy `fun`/`ops` keep rendering as
  "Fun"/"Ops" without a special case. An authored "Play" is unchanged by it.
- **Colours.** `#NAME` goes through `kd_pal_find` (case-insensitive, trimmed). If
  it's omitted, unknown or empty, the header uses `CLAUDE_CORAL`, the only header
  colour before this sprint. An unknown colour warns and falls back. It never
  costs the section its modes, because a header colour is cosmetic. A section
  repeated with a different colour keeps the first one and warns, which matches
  how duplicate ids already behave.
- **Count.** One to three sections. `modeset_section_capacity()` gives 18 / count
  tiles per section (18, 9 or 6), and the Menu always uses three rows, so a single
  section is 6×3, two are 3×3 and three are 2×3. A fourth section warns and is
  dropped (this was already true). A section holding more modes than its capacity
  keeps them on the swipe cycle, and the parse warns that the Menu shows only
  the first N.
- **Layout.** `menu.c`'s `grid_geom()` derives the columns from the count. It uses
  `TILE_W` (288) unless the columns, gaps and dividers would overflow the display.
  Three sections at 288 overflow by about 20 px, so their tiles narrow to 285.
  One and two sections both get 288.

**Regression test: the existing two-section config renders identically.** Before
and after `push-dev`, rpidash2's own `fun:…;ops:…` menu screenshot is **pixel
identical** (PIL `ImageChops.difference(...).getbbox()` is `None`). Host-side,
`test_legacy_spec_unchanged` pins the same thing at the parser level: the same
names, coral headers and capacity 9.

To check the layouts no board's config uses, I temporarily overrode `ExecStart`
on rpidash2 with a volatile `/run/systemd/system/kdeskdash.service.d/` drop-in.
That covered a three-section spec, a one-section spec and `Fun#NOPE:…`. All three
screenshots rendered as designed, and the journal showed the unknown-colour
warning. Afterwards I removed the drop-in, ran daemon-reload, and restarted onto
the unit's own `ExecStart` (confirmed with `systemctl cat`).

Tests are in `tests/test_modeset.c`, written first (red at link time). They cover
the legacy spec, arbitrary names, colours, unknown and empty colours, the
first-colour rule on a repeat, capacity by count, and an over-long name. The old
"unknown section skipped" test became "nameless section skipped", because an
unknown name is now legal.

## Overseer rulings

On the two calls I raised (korg:4370 comment 4378): the first-letter capitalisation
stays, and the README's syntax note now says outright that a lowercase-first header
is not possible. The 285 px tiles in the three-section layout are accepted.

## Docs

The grammar and its degradation paths are documented in the README's per-device
mode sets section and env table, `deploy/kdeskdash.env.example` and `CLAUDE.md`'s
modeset paragraph. The deploy/hosts files are unchanged, since both boards keep
their `fun:`/`ops:` lines.

## Repaired in passing

- **`DUSK_SILVER`'s usage note claimed a use that no longer existed** ("clock: local
  seconds"; nothing referenced it after sprint 036). The merge drops it anyway. I'm
  recording it because the palette mode had been describing a colour no screen used.

## Deployed

2026-11-09 PST, from merged `main` (`18519c4`, PR #53), via `.sprint-deploy`. All
probes were run from kai, which is the deploying host.

- **`deploy-panels`**: `just publish` gave **`0.27.0-18519c4`** (and `latest`).
  `just deploy` pinned both boards to it.
  - **rpidash2**: `--version` prints `kdeskdash 0.27.0-18519c4`, and the unit is
    active. This took it off the sprint's push-dev build. A `kddss` frame of the
    menu is byte-identical to the pre-sprint screenshot, so the legacy `fun`/`ops`
    regression still holds on the published bytes.
  - **rpidash3**: reachable. `--version` prints `kdeskdash 0.27.0-18519c4`, the unit
    is active, and a `kddss` frame shows the launcher with the new clock colours.
- **`recipe: publish-publisher`**: nothing to publish. `kdeskdash-publisher
  2.4.0-c44430a` is already in the store, because this sprint didn't touch the
  publisher.
