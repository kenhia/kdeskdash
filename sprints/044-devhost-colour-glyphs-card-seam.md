# Sprint 044 — Dev-host row colour, glyph coverage, and the service card's failure path

**Proposal:** korg:3234 (a slice of program korg:3245, "Low-hanging fruit — run 3").
**Covers:** WI 2657 (agent glyphs render as boxes), WI 2365 (colour dev-host rows),
WI 2281 (service card's silent-failure path unproven), and, from round 2, WI 3275 (the
TinyTTF journal flood found here).
**Branch:** `044-devhost-colour-glyphs-card-seam`, off `5fed4e3`.

This is an overseen sprint: it ran as karc leg `kdeskdash-d84edd` on kai, and the
overseer's review happens on the proposal thread.

## Premise check

- **2365 holds.** `kvscf:instances:cleo` on central carries `ext_dev_host` on every
  row today (read live with `redis-cli` from kai; all `false`, since no dev host was
  open). kctrldeck publishes it now, and kvscf is retired on cleo.
- **2657 drifted, same direction.** kpidash generated its own Montserrat *Bold* with
  lv_font_conv, but kdeskdash used LVGL's built-in Montserrat (ASCII, `°`, `•` and
  the `LV_SYMBOL_*` set) with no generator at all. So I adapted rather than copied (below).
- **2281 holds.** No test drove the failure path, and the live criterion had never
  been run.
- **The brief's khlenv note is moot.** accept-dns is on for rpidash2 (`CorpDNS: true`,
  resolv.conf points at `100.100.100.100`, rpi53 resolves over MagicDNS), but the panel
  has no khlenv client. It's unmanaged, and its only binary is `kdeskdash`, so there's
  no fallback to watch either way.

## WI 2657: glyphs agents type

**Adapted, not copied.** kpidash's fonts are a different face and weight, and
kdeskdash's text uses LVGL's built-in *Medium* Montserrat. So `fonts/generate.sh`
runs **LVGL's own built-in-font recipe**
(`lib/lvgl/scripts/built_in_font/built_in_font_gen.py`), with the same TTF and the same
FontAwesome symbol list behind `LV_SYMBOL_*`. It reads both from the pinned submodule
and uses the same flags, so nothing is vendored twice. The one thing that changes is
**kpidash's RANGE**: ASCII, Latin-1, en/em dash, curly quotes, bullet, ellipsis. The
two dashboards therefore agree on what an agent can type.

To check the recipe really is LVGL's, I regenerated the built-in 20 px at LVGL's own
range and compared it with `lib/lvgl/src/font/lv_font_montserrat_20.c`. Glyph metrics,
cmaps and kerning came out identical. Of the bitmap bytes, 90 lines differ by ±1 in a
4-bpp alpha value, which is rasteriser drift in lv_font_conv versions and invisible.

- The generated fonts are `kd_font_montserrat_{14,20,28,36,48}.c`: the five sizes
  `src/` uses. They're committed, so a cross-compile needs no Node.js.
- The built-ins are **off** in `lv_conf.h`. `LV_FONT_CUSTOM_DECLARE` declares the new
  ones, `LV_FONT_DEFAULT` points at `kd_font_montserrat_20`, and the 94 references in
  `src/` were renamed mechanically (`lv_` → `kd_`, same length, so no reflow). The
  launcher's `font_has_glyph` filter asks the real font, so it widened on its own.
- **Missing glyphs log once.** `src/logfilter.c` is kpidash's, ported unchanged
  (along with its test). `main.c` registers the print callback after `lv_init()`, and
  `LV_LOG_PRINTF` is 0, because LVGL's printf path is additive with the callback.
- **Gate:** `tests/font-coverage.sh`, which is kpidash's `test_font_coverage.sh`
  extended in two ways. It reads `SYMBOLS` and `SIZES` as well as `RANGE` from
  `generate.sh`, since a dropped symbol here is an `LV_SYMBOL_*` the panel used to
  draw. And it pins U+2014 by name beside kpidash's U+00B0/U+00B7. It runs in
  `just check` as ctest `test_font_coverage`, and `just check-fonts` runs it
  directly. **Negative controls:** widening RANGE by U+2030 without regenerating
  failed all five fonts, and so did adding a symbol to SYMBOLS.

## WI 2365: dev-host rows

- `kvscf_instance_t.ext_dev_host` is parsed with `cJSON_IsTrue`. That means absent,
  `null` and the string `"true"` all read as false, which is kvscf 021's rule.
- A new pure function, `kvscf_row_tone()`, decides the tone. The precedence is
  **muted > dev host > app colour**: a row that isn't running is launchable, not
  focusable, whatever else it says. The wire never pairs `running:false` with a dev
  host, so it's the panel's job not to invent one.
- **Colour call:** `ALARM_EMBER`, taken from the palette through a local
  `COLOR_DEV_HOST` alias in `foreground.c`. I didn't use kvscf's red hex. Its usage
  note now names both uses. A re-tint is one line, either the alias or a new palette
  entry if Ken wants the two meanings apart.
- `test_kvscf_feed` covers the live wire shape, absent → false, a string-valued flag →
  false, and closed-beats-dev-host.

## WI 2281: the service card's failure path

**The seam.** `src/service_pub_internal.h` is modelled on kdashdata's
`kdash_feed_internal.h`. It has five pointers: `ensure`, `set`, `free_reply`, `drop`
and `now`. The real implementations are installed at load, there's a test-only
`service_pub_set_io()`, and nothing in `service_pub.h` changed. `now` is in the seam so
the test owns the clock and never sleeps. `drop` is new: it's the backoff for an error
reply (below).

**It found a defect on its first run**, the way kdashdata's did. I landed the seam
first with the old logic intact, and `test_service_pub` failed 5 checks, all on the
error-reply path. Old code treated any non-NULL reply as a publish, including
`-MISCONF` (rpi53's disk full), `-READONLY` and `-NOAUTH`. So the panel believed the
card had gone out while the kpidash board reddened it. The handle was never dropped,
so AUTH never re-ran. And nothing logged.

**Fix:** an error reply now counts as a failure and calls `redis_client_drop()` (new in
`redis.c`: close, then arm the 5 s backoff), so the next attempt reconnects and
re-AUTHs. Without the drop, the retry-on-next-tick design would write at main-loop
rate, which the test also guards (`persistent failure: at most one write per backoff`).
Failures log **once per outage** with the reason (`no reply` or the server's message),
and recovery logs once too.

`test_service_pub` drives the real tick loop at 50 ticks/s against a fake that fails
the Nth write both ways (no reply / error reply). It asserts:

- one write per 15 s interval in steady state;
- one failed write, then silence through the backoff;
- a failure is never counted as a publish;
- recovery the moment the backoff ends, not 15 s later;
- no writes at all while unreachable;
- ≤ 1 write per backoff window under persistent failure;
- every reply the seam made, the seam freed;
- `shutdown` writes `down` once, or nothing when unreachable.

### Live pass: rpidash2, rpi53:6379 dropped

I ran this from **kai**, the host that does the work, over `ssh ken@rpidash2`, on the
044 dev build (`just push-dev`, `0.27.0-4229c03`). The rule was
`iptables -I OUTPUT -d 100.94.57.102 -p tcp --dport 6379 -j DROP` (rpi53 resolves
IPv4-only on the panel). It was removed by an EXIT/INT/TERM/HUP trap in the session,
**and** a `systemd-run --on-active=4min iptables -D …` safety timer on the panel. After
the test, the rule was gone and the timer had been stopped. The panel was held for 80 s,
from 19:24:17Z to 19:25:37Z, under `strace -e connect,ioctl -T` on the kdeskdash
process.

| check | result |
|---|---|
| panel keeps rendering | 39 `DRM_IOCTL_MODE_ATOMIC` commits across the 79 s window. The claude screen repaints every ~2 s; the median gap was 2.018 s and the **worst 2.520 s**. 10 of 38 gaps ran past 2.1 s: one or two ≤ 250 ms connect timeouts landing in a frame. Rendering was never frozen. |
| publish path backs off, doesn't block | 41 `connect()` attempts to :6379 in 80 s, each non-blocking `EINPROGRESS` and bounded by the 250 ms poll. The card logged `publish failed (no reply)` **once** at 19:24:31Z, the first publish due after the drop. |
| recovery is automatic | `service card publishing again` at 19:25:39Z, **2 s** after the rule came out. The card's `ts` on central read `1790364339` (19:25:39Z). |
| mode switch via the Redis panel-control path | `kdash-pub set kdash:panelmode:rpidash2 '{"mode":"foreground"}'` at 19:26:18Z. The panel's state file read `active_mode=foreground` within ~1 s, and a `kddss` screenshot showed the Remote grid rendered from central. Then switched back to `claude`, where it started. |

**The conflation, stated plainly.** Dropping rpi53:6379 cuts every central handle, not
just the card's: kvscf (rpidash2 reads cleo's deck from central), the command feed, the
claude feed and telemetry. So the stalls above are all of them together, and the
mode-switch check could only run **after** recovery: the panel-control path *is*
rpi53:6379. What the pass proves is the stronger claim: no central handle, the card
included, freezes the panel while its endpoint is gone. The trace also shows the two
libkdash handles doing a MagicDNS lookup (`connect` to `100.100.100.100:53`) before
each reconnect, while the `redis_client_t` handles use their cached IP.

### Live glyph check

I published a throwaway `claude:session:kai:glyphtest-044` on central, with title
`em — en – "curly" 'q' … · café ±½`, status `blocked` so it sorts first, and a 600 s
TTL as a safety. Then I switched the panel to claude and took a screenshot. **Every
character drew as a real glyph**, and the journal showed zero `glyph dsc. not found`
lines. The key was deleted afterwards (`DEL` returned 1).

## Repaired in passing

- `publisher/README.md` said to install `kdash-pub` on cleo with kdashdata's
  `just deploy-cleo`, which no longer exists. kdashdata sprint 018 folded it into
  `just deploy`, one `knarr deploy kdash-pub` across the fleet (checked against
  kdashdata's justfile). Carried over from korg:3233 as proposal comment 3097. README
  isn't in the publisher bundle's payload, so the bundle version doesn't move.

## WI 3275: the TinyTTF "cache not allocated" flood (round 2)

I found this during the live pass and filed it. The overseer then ruled it a repair in
this sprint (option 1, fix the cause) and declined a logfilter dedupe, which would have
hidden the fault and broken logfilter's tested contract.

**Diagnosis.** I temporarily changed `lv_tiny_ttf.c:252` to log `unicode_letter` and
the font pointer, and printed foreground's two font pointers. It was never committed:
the submodule was reverted with `git checkout` and the print removed. A
`kdash:panelmode` switch into foreground and then icons gave codepoints
**U+54 U+65 U+78 U+74, "Text"**, which is LVGL's default text for a new `lv_label`.
The breakdown: 225 lines on foreground's `mark_font`, 13 on its `rail_font`, and 4
each on icons' four preview fonts. `lv_obj_set_style_text_font()` refreshes the label
immediately, so applying a cached Nerd font before setting text measures four Latin
letters the font doesn't have, and each one fails glyph-cache creation. So the fault
wasn't a missing Nerd glyph or cache sizing: nothing about memory, no trade-off.

**Fix:** set the text before the font, at the three sites that didn't. Those are
`foreground.c`'s `make_app_icon` and `mark` labels, and `icons.c`'s preview column.
`icons.c`'s grid cells already did this, with a comment saying why. That's why the
preview column's miss is only 4 lines per font against the mark labels' 225 (28 cells).
I swept every `lv_obj_set_style_text_font` in `src/`; only those two modes hold
TinyTTF fonts. CLAUDE.md's Fonts note now says "per miss" means any lookup, not just a
probe.

**Proof** (rpidash2, build `0.27.0-cd1d49a` via `push-dev`, fresh process): switched
through foreground → icons → foreground → icons → launcher → claude with
`kdash:panelmode`, then a second pass through icons and foreground with a `kddss`
screenshot of each. That produced **zero** `cache not allocated` lines and zero
`[Error]` lines, where the old build gave 236 on one foreground switch. Both
screenshots render as before, and the panel is back on `claude`.

**No regression test,** because this isn't reachable natively. It's the order of two
LVGL calls on a live label, and this repo's tests link only pure cores and never LVGL
(CLAUDE.md). The CLAUDE.md gotcha is the guard; the live count is the proof.

## Gate note

One pre-commit `just check` in round 1 failed in ctest, and I didn't capture which
test: the output went through `tail`, which also hid the exit code, and the commit went
ahead on it. It passed on 30 consecutive runs afterwards, and in round 2 on **10 more,
run unfiltered with each log kept** (rc 0 and 27/27 every time). **It's an unreproduced
single failure**, and the test that failed is unknown.

## Deployed

2026-09-25, from merged `main` (`952ef86`, PR #51), as `.sprint-deploy` declares.

- **`deploy-panels`**: `just publish` published **`kdeskdash 0.27.0-952ef86`** (`latest`
  moved). `just deploy <host> 0.27.0-952ef86` installed it on both boards.

  | board | installed `--version` | unit | frame (`kddss`) |
  |---|---|---|---|
  | rpidash2 | `kdeskdash 0.27.0-952ef86` | active | claude screen, rendered from central; 0 `[Error]` / `cache not allocated` lines in the journal after the restart |
  | rpidash3 | `kdeskdash 0.27.0-952ef86` | active | launcher with its clock pane |

  rpidash2 is off the 044 dev build and onto the published one. rpidash3 was
  reachable this time, so the fleet is level. After the restart rpidash3's launcher
  reads "no launcher configured", because its kvscf Redis held no `kvscf:*` keys at
  that moment (kwork wasn't publishing), and the last-good cache is in memory, so it
  doesn't survive a restart. The layout returns when kwork next publishes. That's
  normal restart behaviour, not a 044 change.
- **`recipe: publish-publisher`**: `nothing to publish: kdeskdash-publisher
  2.4.0-c44430a already in the store`. That's a deliberate no-op: the bundle's payload
  is unchanged (the README repair isn't in it).
