---
date: 2026-07-21
topic: palette-mode
---

# kdeskdash `palette` Mode — Named Palette + Living Style Guide

## Problem Frame

Every new mode picks colors by rummaging through other modes' `#define COLOR_*` blocks
("same orange as the Ops label", "the Edge blue from Remote"). That works, but the
vocabulary is locational, not nominal — specifying a color means describing where it
lives. Two deliverables fix this:

1. **A canonical named palette** (`src/palette.h`) — every deliberate color in the
   design language gets a memorable name, one source of truth.
2. **A `palette` mode** — the living style guide: swatch cards on the actual panel,
   because a color's true value is what the *panel* shows, not what the hex says on a
   monitor. After this, "make the label CLAUDE_CORAL" is a complete instruction.

Like `icons` and `calc`, entirely local: no Redis, no network, pure core + thin mode.

## What's in the palette (and what isn't)

Inventory of today's `lv_color_hex` uses splits three ways:

- **The design language** — the 8 shared core colors (VOID bg, DEEP_SLATE panel, ...,
  MOON_INK, STEEL_MIST, FADED_DENIM), the mode accents (CLAUDE_CORAL, EDGE_TEAL,
  INSIDER_MINT, CODE_BLUE, ROCKET_RED, STAR_GOLD, ZOMBIE_RUST, ...), the status ladder
  (WORKING_JADE, PATIENT_AMBER, ALARM_EMBER), calc's keypad shades, and dev's chart
  family (CPU_SKY, GPU_GRASS, RAM_SALMON, VRAM_MANGO, TOP_LILAC). **~30 colors. These
  are the palette.**
- **Legacy strays** — clock's pre-design-language shades (0xb9c6db, 0xcfe0f5, 0x6ddf6d,
  0x7c93b3) and a few inline one-offs. **Excluded** — they're migration debt, not
  vocabulary. Follow-up WI: repaint clock/dev onto palette names and delete them.
- **Sim pixels** — gol/golz canvas colors are composed per-pixel in the pure sim cores.
  Not UI vocabulary; excluded.

Names are paint-store style (evocative, unambiguous when spoken) with the mode/role
hinted where it helps: `CLAUDE_CORAL`, `BURNT_CORAL`, `EDGE_TEAL`, `GUNMETAL_SEAM`,
`SCORCHED_WASH`. The mode shows a usage note per swatch so the name→role mapping is
self-documenting.

## The single source of truth: X-macro table

`palette.h` defines the table once; the enum, the rgb lookup, and the display table
all generate from it — adding a color is one line:

```c
#define KD_PALETTE(X) \
    X(VOID,          0x05070d, "app background") \
    X(DEEP_SLATE,    0x0a0f1a, "panel / card fill") \
    X(CLAUDE_CORAL,  0xcf6b4a, "accent: claude, Ops header, calc hex") \
    ...
```

Modes keep their local semantic defines for now (`#define COLOR_ACCENT
kd_pal(KD_PAL_CLAUDE_CORAL)` when touched) — no big-bang rewrite; migration is the
follow-up WI, done opportunistically as modes are edited.

## Layout

1920×440, design-language cards. **4×2 grid of swatch cards** + a narrow right rail
for paging (the icons-mode pattern — this palette won't fit one screen, ~30 swatches
= 4 pages of 8):

```text
+----------------+----------------+----------------+----------------+------+
| CLAUDE_CORAL   | EDGE_TEAL      | INSIDER_MINT   | CODE_BLUE      |  ^   |
|  accent: ...   |  Remote Edge.. |  Remote green..|  VS Code blue..| Prev |
|  Handgloves 019|  Handgloves 019|  Handgloves 019|  Handgloves 019|      |
|  [##] [==] #hex|  [##] [==] #hex|  [##] [==] #hex|  [##] [==] #hex| 2/4  |
+----------------+----------------+----------------+----------------+      |
| ...            | ...            | ...            | ...            | Next |
|                |                |                |                |  v   |
+----------------+----------------+----------------+----------------+------+
```

Each card:
- **Name** — montserrat_28, *in the color* (the "bold" sample; see font note)
- **Usage note** — montserrat_14, secondary; what it means / where it's used
- **Sample text** — montserrat_20 in the color: `Handgloves 0123456789`
- **Filled box** (hairline-bordered so near-black surface colors stay visible against
  the card) + **outlined box** (3px border in the color, transparent fill) + **hex**
  (`#cf6b4a`, secondary — always readable even when the color itself vanishes on slate)

Dark surface swatches (VOID on a DEEP_SLATE card) will show near-invisible text — that
is honest: they're backgrounds, and their boxes + hex row carry the card.

### The bold-text caveat

Built-in LVGL Montserrat is a single weight — there is no bold, and the vendored Nerd
Font TTF has no Latin glyphs. The 28px name serves as the heavyweight sample next to
the 20px text. True bold (Montserrat-Bold, kpidash-style bake) is deferred; if it ever
lands, the card gains a real bold line.

## Pure core / thin mode split

- `src/palette.c/h` — the X-macro table, `kd_pal_count()`, `kd_pal_name(i)`,
  `kd_pal_rgb(i)`, `kd_pal_usage(i)`, `kd_pal_find(name)` (case-insensitive, so
  "claude_coral" works spoken-style). Host tests: names unique and `[A-Z0-9_]`,
  rgb within 24-bit, find hits/misses, count matches enum.
- `src/modes/palette.c` — cards + rail, page state, gesture guards + GESTURE_BUBBLE
  everywhere (button-dense screen, same as calc). Prev/Next wrap. No tick.

## Open questions for the live test

1. 8 cards/page — right density, or drop to 6 bigger cards?
2. Is the usage note earning its space, or should the sample text line be longer?
3. Do the near-black surface swatches need a light well behind the boxes?
4. Which names miss ("that's not what I'd call that color") — rename freely now,
   they're not load-bearing until modes migrate.
