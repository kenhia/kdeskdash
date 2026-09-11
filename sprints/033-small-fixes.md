# 033 — Small fixes: Windows publisher install, the service card, double-tap, the palette migration

korg: proposal 2228, one leg of program 2233 (backlog drain 3 of 3, the
sub-1000 sweep). Covered: 800, 782, 1032, 514 — all four landed. 847 was
investigated here, found blocked on a question only Ken can answer, and
**detached from the proposal by the overseer** so a merge-ready branch would not
hold the kdeskdash repo lock (three more slices are queued on it) waiting for a
human. Run headless as karc leg `kdeskdash-3e9ab5`.

The proposal's standing rule was **verify before working**: these items are 6–8
weeks old and two were suspected moot. None were. The premise check is written
up first because it is the part that would have been wasted work.

## Premise check

| item | verdict |
|---|---|
| 800 Windows `.sh` hook | **holds, narrowed.** README install step 2 and `settings-fragment.json`'s `//path` still name a bare `.sh`. The *scheduled task* half has since grown an explicit `bash.exe` (the `poll-hidden.vbs` shim), so only the hooks/statusline path was still wrong. |
| 782 service card | **holds.** Nothing in `src/` mentions `kpidash:services`. Not moot. |
| 514 palette | **holds.** 74 local `COLOR_*` defines across 7 modes, plus 18 inline literals. |
| 1032 double-tap | **holds.** `shell.c` handles LEFT/RIGHT/BOTTOM only; LVGL 9.2.2 has no double-click event at all. |
| 847 ABS doc link | **holds, but blocked.** See "847 — verified, not landed". |

## What changed

**800 — the Windows hook command names its interpreter.** A bare `.sh` path only
runs if the launcher routes through Git Bash; through PowerShell it reaches
ShellExecute, and `.sh` has no handler, so Windows pops a modal *"How do you
want to open this file?"* picker. It fails unattended and silently — the desktop
app re-spawns Claude Code on its own, every warm-up fires SessionStart, and the
transcript still records `hookErrors: []` because the launch "succeeded". The
install step and the fragment now both say
`C:/PROGRA~1/Git/bin/bash.exe …/claude-pub.sh hook`, and a new README section
carries the *why* — including the shell-coverage table and the warning not to
dismiss the picker by choosing an app, which registers a permanent `.sh`
association and makes the symptom vanish while the bug remains. Linux hosts were
never affected, so no k-homelab change was needed.

**782 — every instance self-publishes a kpidash service card.** Pure core
`service_card.c` (key shape, payload, JSON escaping, throttle) + thin publisher
`service_pub.c` on its own handle. Write-only; kdeskdash never reads
`kpidash:services:*`.

**1032 — double-tap the background to switch modes.** Pure core
`quickswitch.c`; `shell.c` wires it. `KDESKDASH_QUICK_PAIRS` has three reachable
states: unset (partner = previously active mode), `none`/`off` (inert), or
pinned pairs.

**514 — the palette is now the single source of truth.** All 74 `COLOR_*`
defines and all 18 inline literals across 10 modes now resolve through
`src/palette.h`. **There is no bare `lv_color_hex(0x…)` left in `src/`.**

## Things worth keeping

**A new endpoint gets the auth rule that already cost a sprint.** The card
writes to the same Redis the telemetry feed reads, so `KDESKDASH_CARD_REDIS_*`
falls back to `KDESKDASH_TELEMETRY_REDIS_*` — host and port independently, and
**auth only when host *and* port both match**. That is sprint 031's lesson
(`config.c`'s kvscf comment) applied to a new handle on the day it was added,
not after it broke: a Redis with no password answers AUTH with an *error*, so an
inherited password reads on the panel as an endpoint that is simply down. The
payoff is that neither Pi needs a new env line — both already point telemetry at
`rpi53:6379` with the password in `secrets.env`, so the card inherits a working,
authenticated endpoint for free.

**The contract was verified against the side that actually connects.** Rather
than trusting the handoff, the real board was read: kpidash's own self-card is
`{"ts":…,"state":"ok","text":"0dc5bca (2026-09-05)","host":"rpi53","icon":20}`
with **TTL −1**, and `kpidash:services:klams:_` proves the `_` sentinel is live.
The builder's output was then parsed alongside it — identical field set, matching
types. The unit test pins that shape, because this contract's failure mode is
*silence*: a 3-segment key or an unparseable payload renders no card and logs
nothing.

**Double-tap is read on the bare background, and that is the safety property.**
LVGL does not bubble `CLICKED` to a parent, so a handler on the *screen* cannot
fire for a tap on a calc key, a launcher button or a dev row. Modes keep their
own taps unchanged. The handler still takes the swipe-vs-tap guard — a swipe
that releases over the background fires `CLICKED` too
(`docs/solutions/best-practices/lvgl-swipe-vs-tap-gesture-guard.md`).

**The quick-switch partner works with no configuration — and can still be turned
off.** `KDESKDASH_QUICK_PAIRS` pins a partner as the item asked, but unset is a
*working default*, not "off": the partner is the previously active mode. For the
Claude↔Remote case that prompted the item, that is the same behaviour with
nothing to set up. **The menu is deliberately excluded from that history** —
passing through it to reach a mode must not make it the partner, which is the one
thing that would have made the zero-config path useless.

That fallback originally left the gesture *unconditional*: there was a way to
change the partner and no way to decline the feature, and **a default you cannot
opt out of is not a default** (overseer ruling 2). `none` — or `off`, because a
reader who knows `env_flag`'s vocabulary will try it — now disables it outright.
The grammar lives in `quickswitch.c`, not `config.c`, the same split `modeset.c`
has with `KDESKDASH_MODES`; `config_load` passes the spec through verbatim, and
`test_config` pins that the three states stay distinguishable. A mode genuinely
named `none` on one side of a pair is still a pair.

**Migrating 92 colors changed 3 of them, all below the JND.** Every value was
matched by exact RGB where the palette had one (67 of 74 defines). The rest were
measured in CIELAB: only `0xeaf0fb`→`MOON_INK` (ΔE 1.54, twice) and
`0x1b2433`→`GUNMETAL_SEAM` (ΔE 1.62) were collapsed. The other 12 strays got
**new palette entries** rather than being repainted onto a near neighbour —
adding a name changes no pixels, and which near-duplicates *should* merge is a
design call for Ken on the panel, filed as **WI 2259**.

**A fifth Redis handle, and the doctrine that names them was updated in the same
sprint.** The card writes to `rpi53:6379` — telemetry's endpoint — but telemetry
is initialised only when Dev mode is registered, and the card must publish from
every panel, so sharing would tie a panel's presence on the kpidash board to
whether it happens to carry Dev. Conflating them would have been the actual
violation of "do not conflate them"; the rule is against *sharing*, not against
*existing*. `CLAUDE.md` said **"Four independent Redis handles"** and now says
five, with the service card documented and — the load-bearing half — *why it is
not telemetry*, so the next reader who notices two handles pointing at one
endpoint does not merge them. Shipping a sprint that falsifies its own governing
doctrine is the same decay this whole program exists to reverse.

**`#include "palette.h"` from inside `src/modes/` is a trap, and it is already
documented.** `src/modes/palette.h` (the mode header) shadows the core header,
so the quoted include silently resolves to the sibling. The repo already had
`docs/solutions/best-practices/quote-include-core-header-shadowing.md` and
`launcher.c` carries the comment; all ten migrated files now carry it too.

**Two palette assertions were re-stated, not relaxed.** `TRUE_BLACK` is
genuinely darker than `VOID`, so "VOID first" became "darkest neutral first,
VOID second". And the blue family's *fixed 4-card window* was only ever a proxy
for "blues together, nothing foreign between them" — `CAPTION_HAZE` crossing
`NEUTRAL_CHROMA` into the blue bin widened the run without scattering it. The
test now asserts the invariant (everything between the family's ends is
blue-dominant), which does not re-break every time the palette grows.

## Gates

`just check` — **21/21 green**, up from 19: `test_service_card` and
`test_quickswitch` are new, and `test_config` gained five cases (four for the
card's endpoint/auth fallback, one pinning the quick switch's three states). The
host `kdeskdash` binary builds warning-clean.

**Not verified here:** the aarch64 cross build. There is no `~/pi-sysroot` on
kai, so `build-pi` could not be configured — the new sources are POSIX-only C
(`unistd.h`/`gethostname`), but the first real exercise is `just publish` at
deploy. Likewise the card's live acceptance criteria (AC-1…AC-6) need the binary
*running on a Pi*; nothing was written to the live board from here, deliberately,
because kpidash keeps a card in an in-memory registry until restart and a test
card would have needed a proper evict to remove.

## 847 — verified, not landed, and no longer this proposal's

`stl/README_ABS.md` is still in `.scratch/` and still unlinked, correctly. The
item is gated on a question only Ken can answer: **which field did `100.545 %`
go into?** Filament Settings → Shrinkage is XY-only *and* takes the shrink not
the scale (so the value would be `99.46`, and the trial print is invalid twice
over); the per-object Z scale is the right mechanism and the measurement stands.
The "Still to verify" checklist in the notes is untouched since 2026-08-01, so
the 2026-07-31 trial result was never recorded. Marked **Awaiting Ken** with the
question on the item (comment 1731) rather than shipping a doc link whose central
claim is unverified — and linking it now would also read as "ABS is ready", while
the blind overhang is still open.

The overseer then **removed the `covers` edge**: holding a merge-ready branch for
a human answer would serialise the kdeskdash repo lock behind Ken's sleep, and
slices korg:2218, korg:2231 and korg:2219 are all queued on that one lock. 847
goes back to the kdeskdash backlog, `open` and Awaiting Ken, on this push's
batched-ask list. Do not re-relate it to this proposal.

## Follow-ups filed

- **WI 2259** — which near-duplicate palette neutrals should merge (design call,
  verify on-panel with the `palette` mode).

## Deployed

**0.27.0-06b6f9a**, published from merged `main` (`06b6f9a`) and installed on
**both** boards — rpidash3 happened to be awake, so the fleet is whole rather
than split.

| board | version reported | unit | frame |
|---|---|---|---|
| rpidash2 (Pi 5) | `kdeskdash 0.27.0-06b6f9a` | active | Claude mode, correct |
| rpidash3 (Pi 4) | `kdeskdash 0.27.0-06b6f9a` | active | Launcher mode, correct |

`just versions` reports both board lines as exactly `kdeskdash 0.27.0-06b6f9a`.
rpidash3's frame shows *"no launcher configured"* — the documented degraded
state when kwork's machine is off, not a regression. The unit file was untouched
this sprint, so no `install-service` was needed.

**The aarch64 cross build was never actually blocked.** The pre-ship note said
`build-pi` could not be configured because there is no `~/pi-sysroot` on kai.
That check was too narrow: the toolchain also honours the legacy
`~/pi5-sysroot`, which exists, and `cmake/aarch64-toolchain.cmake` falls through
to it by design so an old tree keeps working. The cross build ran clean and the
artifact is `ELF 64-bit LSB pie executable, ARM aarch64`.

### WI 782's acceptance criteria, on the live board

| AC | result |
|---|---|
| **AC-1** one key per instance, JSON with `ts`/`state`/`text`/`host` | **PASS** — `deskdash:rpiDash2` and `deskdash:rpidash3`, both parsing, both `TTL -1` |
| **AC-2** a `deskdash` card per instance, green (ok) | **PASS** — kpidash's own `kpidash-cards list` shows both `ok`, ages 5 s and 12 s |
| **AC-3** `ts` refreshed well under the 60 s cutoff | **PASS** — advanced 30 s and 45 s across a 40 s window (the 15 s cadence); ages 11 s and 3 s |
| **AC-4** `text` shows the running version | **PASS** — `0.27.0-06b6f9a` on both |
| **AC-5** a down Redis never crashes or stalls the render loop | **by construction, not exercised live** — see below |
| **AC-6** both dashboards distinguishable on the board | **PASS** — two cards, `(name, host)` identity, no extra config |

**AC-5 is the honest gap.** Both panels render and publish with the endpoint up,
which shows the publish does not stall the loop in the normal case — but the
endpoint was never taken *down* under a running panel, so the failure path is
evidenced by construction only: `service_pub_tick` returns early when
`redis_client_ensure` fails, the handle carries its own backoff, and `g_last` is
deliberately not advanced on failure so a reconnect publishes immediately. That
is the same shape as the four older handles. Worth a real test the next time
something touches this path.

### One finding

rpidash2's actual hostname is **`rpiDash2`** (capital D), so its key is
`kpidash:services:deskdash:rpiDash2` while every other fleet reference is
lowercase. Nothing is broken and both cards render — but the key *is* the card's
identity and cards have no TTL, so a later lowercasing would create a second
card and strand the first. Filed as **WI 2277** with the prune step named.
