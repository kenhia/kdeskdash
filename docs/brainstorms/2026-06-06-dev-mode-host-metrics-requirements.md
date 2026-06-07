---
date: 2026-06-06
topic: dev-mode-host-metrics
---

# kdeskdash `dev` Mode — Dual-Host Dev Metrics Panel

## Problem Frame

The MVP shell ships ambient/utility modes (Game of Life, Clock) and treats Redis as
optional. `dev` is the **workhorse** mode the appliance was really built for: an at-a-glance
view of development-machine load (CPU/RAM and GPU/VRAM) for two hosts side by side, picked
from the live fleet of machines already publishing telemetry to kpidash's Redis.

This realizes the dev/usage-graph vision deferred from the MVP (origin:
docs/brainstorms/2026-06-06-multimode-dashboard-premvp-requirements.md), which the MVP
substituted a Clock for because the client data pipeline wasn't ready. That pipeline now
exists: kpidash already runs a Redis instance (host `rpi53`), a self-registering client
fleet, and a per-host `dev_telemetry` payload. `dev` mode is therefore mostly an **LVGL
view + host selector** over data that already flows — it does not define a new telemetry
contract.

Introducing `dev` changes one core posture: **Redis becomes required**, and kdeskdash now
talks to **two** Redis endpoints — its existing local instance (control + persistence) and
kpidash's remote instance on `rpi53` (telemetry).

## Layout

The display is wide and short (native 1920×440 on `rpidash2`). The panel is four graphs
flanking a center host selector. GPU/VRAM sits toward the center on **both** sides
(mirrored), because GPU/VRAM is the primary focus; CPU/RAM sits on the outer edges.

```text
 LEFT HOST                       CENTER                      RIGHT HOST
+-----------+-----------+   +---------------+   +-----------+-----------+
|  CPU/RAM  |  GPU/VRAM  |  |    [ <-- ]     |  |  GPU/VRAM  |  CPU/RAM  |
|  (outer)  |  (center)  |  |  host list:    |  |  (center)  |  (outer)  |
|           |            |  |   cleo         |  |            |           |
|  ~~line~~ |  ~~line~~  |  |   kai   *sel*  |  |  ~~line~~  |  ~~line~~ |
|           |            |  |   kubs0        |  |            |           |
|           |            |  |   kubsdb  (v)  |  |            |           |
|           |            |  |    [ --> ]     |  |            |           |
+-----------+-----------+   +---------------+   +-----------+-----------+

Assign flow: tap a host name (highlights as selected) -> tap [ <-- ] to place it
on the LEFT pair, or [ --> ] to place it on the RIGHT pair. List scrolls when it
exceeds the visible rows.
```

Prose is authoritative: each host occupies a two-graph pair; the GPU/VRAM graph of each
pair is the inner graph (nearest the selector), CPU/RAM is the outer graph; the selector is
the fixed center column.

## Requirements

**Data Source & Contract**
- R1. `dev` reads host metrics from the same Redis instance kpidash publishes to (remote
  host `rpi53`), which is **separate** from kdeskdash's local control/persistence Redis.
- R2. Selectable hosts are discovered **live** from kpidash's client registry (the
  `kpidash:clients` Set); a host appears when its client starts publishing and is treated as
  gone when it stops.
- R3. Per-host metrics are consumed from kpidash's existing `dev_telemetry` payload (CPU %,
  top-core %, RAM used/total, and optional GPU compute %, VRAM used/total). `dev` consumes
  the existing schema as-is and does **not** define a new telemetry contract.
- R4. Enabling `dev` makes Redis a **hard dependency** for kdeskdash (previously optional).
  When the telemetry Redis is unreachable, `dev` shows a clear unavailable state rather than
  crashing or hanging the app; other modes remain usable.

**Graph Rendering**
- R5. Each host renders **two separate** graphs — a CPU/RAM graph and a GPU/VRAM graph — not
  a single combined chart. This is the deliberate divergence from kpidash's combined view.
- R6. Graphs are rolling time-series consistent with kpidash's baseline (line charts, fixed
  time window, shift-to-scroll update). Plain, readable lines are an acceptable baseline.
- R7. The GPU compute series renders as a **thick line**, reusing kpidash's proven
  triple-trace offset technique (LVGL 9 has no per-series line-width API).
- R8. VRAM rendered as a **filled area** under its line is a **stretch goal**; if filling
  proves non-trivial it ships as a normal line and is deferred to a follow-up.

**Selector & Assignment UX**
- R9. The center column is a compact host selector listing the live clients, with
  prev/next (`[ <-- ]` / `[ --> ]`) controls and scrolling when the list exceeds the visible
  rows.
- R10. Assignment interaction: tap a host name to select it (it highlights), then tap the
  left-assign control to place it on the left pair or the right-assign control to place it on
  the right pair. Names are the pick list; the arrows are the commit/assign action.
- R11. The selector indicates each host's current assignment (e.g. an L/R marker or styling on
  assigned rows) so it's clear at a glance which host is shown on which side.
- R12. Assigning a host to a side replaces whatever host was on that side. The same host may be
  assigned to both sides simultaneously (harmless mirror view); assignment does not remove the
  host from the selector list.

**Layout & CPU-only Handling**
- R13. Layout is four graphs flanking the center selector, with GPU/VRAM toward the center
  on both sides (mirrored) and CPU/RAM on the outer edges (see Layout diagram).
- R14. When an assigned host has **no GPU data** (`gpu` is null/absent), its side shows a
  normal-sized CPU/RAM graph **centered** within that side's full two-graph footprint —
  consistent graph sizing, balanced empty space, no stretching or lopsided placement.

**Persistence & Liveness**
- R15. Left and right host assignments **persist** across restarts via kdeskdash's local
  control Redis (same mechanism as the active-mode restore) and are restored on boot.
- R16. When an assigned host goes **stale** (telemetry TTL expires / it stops publishing),
  its graphs show a "no new data" overlay and freeze the existing history so context stays
  visible; they auto-recover when data resumes (kpidash's staleness pattern).
- R17. An **empty** side (nothing assigned yet, or first boot before any pick) shows a
  placeholder prompt (e.g. "Select a host") rather than a blank or broken graph.
- R18. A **persisted host that has vanished** from the client set is kept visible in its slot
  as stale/offline rather than silently dropped, so a dead host is noticeable.

**Display / Hardware**
- R19. Support flipping the entire display **180°** (the screen mounts upside-down in the
  3D-printed case). The flip is global (affects all modes), toggleable via configuration
  (env/config, not a code edit), and must rotate **both** rendered output and touch input
  together. This is validated by an **early spike** so the team can regroup if the display
  driver fights the rotation (see Outstanding Questions).

## Success Criteria
- Two chosen hosts' CPU/RAM and GPU/VRAM load are readable at a glance from across the desk,
  updating live.
- Picking a host and assigning it to a side takes a couple of taps and survives a reboot.
- A host going offline is obvious (stale overlay), and history isn't lost on a brief blip.
- A CPU-only host looks intentional and balanced, not broken.
- The screen reads right-side-up when mounted upside-down in the case, with touch targets
  landing where they're drawn.

## Scope Boundaries
- Not defining or changing kpidash's telemetry schema or client publishing — `dev` is a
  consumer only.
- Not combining CPU/RAM and GPU/VRAM into a single chart — separate graphs is a requirement,
  not a fallback.
- Not building per-host history persistence — graphs are live/rolling; history resets on
  restart (only the host→side assignment persists).
- VRAM area-fill (R8) is explicitly out of the committed scope this sprint (stretch only).
- Not adding more than two host slots; exactly left + right this sprint.

## Key Decisions
- **Reuse kpidash's Redis + fleet** (vs. building a new collector): `dev` is mostly a view;
  the data already flows. Cost accepted: dual-Redis architecture and re-introducing JSON
  parsing into kdeskdash (the MVP dropped cJSON).
- **Redis becomes required for `dev`**: deliberate posture change; only `dev` hard-depends on
  telemetry Redis, and it degrades to an unavailable state rather than breaking the app.
- **Separate graphs, GPU/VRAM toward center, mirrored**: GPU/VRAM is the primary focus, so it
  gets the central, most-visible position on both sides.
- **CPU-only = centered single graph (Option 3)**: keeps every graph the same size for calm,
  consistent reading; lowest implementation cost.
- **Thick GPU line in, VRAM fill deferred**: the thick line reuses a proven kpidash technique
  (cheap); the fill is new work (LVGL 9 has no native area-fill) with uncertain cost.
- **Adopt kpidash's staleness/empty handling wholesale**: stale overlay + frozen history +
  auto-recover, placeholder for empty sides, keep vanished hosts visible as offline.
- **Screen flip in scope, spiked early**: needed for the physical build; small and global,
  but validated up front because the DRM path may not honor software rotation cleanly.

## Dependencies / Assumptions
- kpidash's Redis on `rpi53` is reachable from `rpidash2` and continues publishing
  `kpidash:clients` and per-host `dev_telemetry` in its current schema.
- kdeskdash's local Redis remains available for control/persistence (assignments).
- The display is single-threaded LVGL; all telemetry polling stays on the UI/timer thread
  (no background threads), consistent with the existing redis poll model.

## Outstanding Questions

### Resolve Before Planning
- (none — product decisions are resolved)

### Deferred to Planning
- [Affects R3][Technical] JSON parsing approach for `dev_telemetry` — re-add cJSON (matches
  kpidash) vs. a tiny hand-rolled parser for the known fields. Decide during planning.
- [Affects R1][Technical] How to configure/connect the second (remote `rpi53`) Redis endpoint
  alongside the existing local client (config keys, auth, connect/backoff reuse).
- [Affects R4][Technical] Exact "telemetry unavailable" presentation and how it interacts with
  the empty-side placeholder (R17) and stale overlay (R16).
- [Affects R7][Needs research] Confirm kpidash's triple-trace offset values translate cleanly
  to `dev`'s separate-graph axes (offset is axis-unit based, not pixel based).
- [Affects R19][Needs research] **Early spike:** does `lv_display_set_rotation(disp,
  LV_DISPLAY_ROTATION_180)` work under the `lv_linux_drm` driver (may need full-refresh
  render mode), and does touch follow automatically? Fallback: KMS/kernel `rotate=180` plus
  `lv_evdev` calibration to invert both touch axes. Run this before building out the mode so
  scope can be adjusted if needed.
- [Affects R2/R18][Technical] Default startup assignment when nothing is persisted yet (empty
  both sides vs. auto-assign first live hosts) — lean empty + placeholder, confirm in planning.

## Next Steps
→ `/ce-plan` for structured implementation planning.
