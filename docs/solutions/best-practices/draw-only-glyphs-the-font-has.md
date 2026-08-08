---
title: Filter text down to glyphs the font has — the fallback chain will not save you
date: 2026-08-08
category: docs/solutions/best-practices
module: LVGL UI modes (launcher, clock widget, foreground)
problem_type: best_practice
component: frontend_stimulus
severity: medium
applies_when:
  - Rendering text that arrives from another machine (a feed, a window title, a button label)
  - Text may contain emoji, wide Unicode, or Nerd Font PUA glyphs
  - Reaching for lv_font_t.fallback to cover a font's gaps
  - Adding a decorative separator or symbol to your own chrome
related_components:
  - launcher mode
  - clock widget
  - icons mode (TinyTTF)
tags: [lvgl, fonts, unicode, emoji, tinyttf, montserrat, tofu, nerd-font]
---

# Filter text down to glyphs the font has

## The problem

A mode renders a label that came off the wire — a window title, a Stream Deck
button name. Ken's names carry emoji. The panel draws a replacement box.

The instinct is to reach for a font that covers more, or for LVGL's fallback
chain. On this build both instincts are wrong, and it is worth knowing why
before spending an afternoon on it.

## What is actually in the fonts

Measured, not assumed (`fontTools`, `fonts/ttf/SymbolsNerdFont-Regular.ttf`):

| Range | Coverage |
|---|---|
| Emoji (U+1F300–1FAFF) | **0 glyphs** |
| Variation selector U+FE0F | absent |
| ASCII / Latin | **0 glyphs** |
| Nerd Font PUA (e.g. U+F0A1E) | present, ~10,400 total |

`SymbolsNerdFont-Regular.ttf` is *symbols only*. It has never been able to draw
`🦀`, and it cannot draw `A` either. The built-in Montserrat bitmap fonts cover
ASCII and Latin-1 but no emoji — and not every Latin-1-adjacent character you
might reach for: **U+00B7 (`·`) is missing**, which is easy to trip over when
adding a separator to your own chrome.

## Why the fallback chain does not fix it

`lv_font_t.fallback` works by asking the primary font for the glyph and moving
on when it says no. That depends on `lv_font_get_glyph_dsc` returning `false`
for a missing glyph.

- For LVGL's built-in `LV_FONT_FMT_TXT` fonts (Montserrat), it does. The
  predicate is trustworthy.
- For **TinyTTF it does not** — it returns `true` even for glyphs the face
  lacks. (The same trap the glyph-probing note in CLAUDE.md records: probe with
  a cache-less font and test `dsc.gid.index != 0` instead.)

So a TinyTTF primary with a Montserrat fallback never falls through: Latin
renders blank. And you cannot chain the other way, because the built-in fonts
are `const lv_font_t` — there is no field to set.

A *cached* TinyTTF font also logs `cache not allocated` on every miss, so
chaining one under ordinary Latin text spams the journal once per character.

## What to do instead

**Filter the string to what the font can draw, before drawing it.** Keep the
predicate as a seam so it is testable without a display:

```c
size_t kvscf_label_filter(const char *in, char *out, size_t outsz,
                          bool (*renderable)(uint32_t cp, void *ctx), void *ctx);
```

The mode passes a predicate backed by `lv_font_get_glyph_dsc` on the actual
label font; the unit tests pass a stub. Beyond the predicate, always drop the
codepoints no monochrome renderer can use anyway — ZWJ (U+200D), variation
selectors (U+FE0E/FE0F), skin-tone modifiers (U+1F3FB–1F3FF) — and **collapse
the whitespace the removal leaves behind**, or a dropped leading emoji leaves
the text indented.

`"🦀 Rust Docs"` becomes `Rust Docs`. Not a box, and not `" Rust Docs"`.

Decode UTF-8 strictly on the way through (reject overlongs and surrogates) and
never truncate mid-sequence — the text came from another machine.

## The rule

Anything you draw must be in the font, **including your own chrome**. This note
exists partly because the sprint that added the label filter then rendered a
`·` separator in its own clock widget and logged a missing glyph until it became
two spaces. Stick to ASCII in decoration, and filter anything you did not write.

If emoji genuinely need to render, the honest path is a vendored monochrome
emoji font (Noto Emoji, ~1.5 MB) *plus* a composite `lv_font_t` whose
`get_glyph_dsc` dispatches across faces — because the built-in chain cannot be
driven from a TinyTTF primary. Budget for both, or filter.
