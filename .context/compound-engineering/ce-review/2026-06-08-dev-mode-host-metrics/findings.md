# ce-review run artifact — dev-mode-host-metrics

- **Run id:** 2026-06-08-dev-mode-host-metrics
- **Mode:** interactive
- **Scope:** branch `feat/dev-mode-host-metrics` vs base `cd5e46d` (merge-base with `main`)
- **Diff:** ~1723 non-test src LOC across 31 files (vendored `lib/cjson/`, `docs/` excluded)
- **Reviewers (10):** correctness, security, reliability, maintainability, testing, performance, adversarial, project-standards, agent-native, learnings-researcher
- **Plan:** docs/plans/2026-06-07-001-feat-dev-mode-host-metrics-plan.md (plan_source: inferred)

## Verdict

Ready with fixes. No P0. One P1 (DNS-on-UI-thread) and two P2s warrant follow-up; safe fixes applied in-run.

## Applied fixes (safe_auto, this run)

| # | Sev | File | Fix | Reviewers |
|---|-----|------|-----|-----------|
| 1 | P2 | src/dev_telemetry.c | `clamp_mb` ceiling UINT32_MAX → INT32_MAX so MB values stay safe to cast to the int32 LVGL chart axis (prevents axis inversion / negative plot from hostile/huge totals). Test updated + non-negative assertions added. | correctness + adversarial (agreement, conf 0.88) |
| 2 | P2 | README.md | Added `KDESKDASH_TELEMETRY_REDIS_HOST/_PORT/_REDISCLI_AUTH` to env table + `kdeskdash:dev:left|right` to keys table + `dev` as an active_mode value. | project-standards, agent-native (conf 0.90) |
| 3 | P2 | README.md, deploy/kdeskdash.env.example | Reworded `KDESKDASH_ROTATE_180` docs: parsed-but-currently-a-no-op (matches the neutralized code), was advertised as a working flip. | project-standards (conf 0.76) |
| 4 | P3 | src/telemetry.c | Reset `g_cursor`/`g_building_n` on both discover failure paths so a reconnect starts a clean SCAN pass (no stale-cursor partial host list). | reliability (conf 0.60) |
| 5 | P3 | src/modes/dev_graph.c | Extracted duplicated `16384` MB-axis ceiling into `MB_AXIS_DEFAULT`. | maintainability (conf 0.78) |
| 6 | P2 | src/redis.c | `redis_client_ensure` now arms the reconnect backoff on a *command/read-timeout* error (not just connect-fail), so a reachable-but-slow remote can't thrash connect+timeout every tick and stall the UI. | reliability + performance (conf 0.72) |
| 7 | P3 | src/redis.c | Extracted duplicated GET-into-buffer body into `redis_get_string`; both getters delegate. | maintainability (conf 0.72) |
| 8 | P3 | src/modes/dev.c | Renamed `dev_side_t.cpu_only` → `applied_cpu_only` (disambiguate from gate field); added `side_mark_gap`/`side_set_status` fan-out helpers for the twin-call sites. | maintainability (conf 0.62–0.66) |
| 9 | P3 | src/modes/dev_graph.{c,h} | Removed dead `dev_graph_destroy` (no callers). | maintainability (conf 0.80) |
| 10 | P3 | src/modes/dev.c | Added `_Static_assert(DEV_HOSTLIST_MAX >= TELEMETRY_HOSTS_MAX)` at the merge coupling point. | maintainability (conf 0.60) |
| 11 | P1 | src/redis.h | Documented that the 250ms connect timeout does NOT bound `getaddrinfo`; recommend IP / `/etc/hosts` pin. Full fix tracked as WI #51. | reliability (conf 0.70) |

Verification: `test_dev_telemetry`, `test_dev_view`, `test_dev_hostlist` host suites pass; cross-build clean.

## Tracked follow-up work items (kwi, project kdeskdash, sprint feat/dev-mode-host-metrics)

| WI | Title | From finding |
|----|-------|--------------|
| #51 | Bound DNS resolution so a slow resolver can't stall the UI thread | P1 (DNS, reliability) |
| #52 | Add a "waiting for data" dev-side state for assigned-but-never-live hosts | P3 (adversarial) |
| #53 | Stop leaking hiredis through redis.h to every consumer | P3 (maintainability) |

## Residual actionable work (not auto-applied)

### P1
- **DNS resolution not bounded by connect timeout (UI thread)** — src/redis.c:53. Interim doc note applied (fix #11); full async/at-init resolve tracked as **WI #51**. (reliability, conf 0.70, requires_verification on rpidash2).

### P2
- **Synchronous telemetry GET/SCAN + no-backoff-on-read-timeout** — the no-backoff half is **FIXED** (fix #6). The remaining synchronous-I/O-on-UI-thread aspect is inherent to the single-thread design (bounded by the 50ms read timeout + new backoff) and accepted; revisit only if the DNS work (WI #51) reshapes the connect path. Needs a hardware re-verify against a slow remote.

### P3 (maintainability / design — gated or advisory)
- never-live-but-discovered host renders as flat-zero LIVE forever — src/modes/dev_view.c. Adversarial, conf 0.70. Tracked as **WI #52**.
- `dev_graph_destroy` is dead code — **FIXED** (fix #9).
- Duplicated `ensure→GET→strncpy→NUL→free` body — **FIXED** (fix #7, `redis_get_string`).
- Two same-named `cpu_only` fields — **FIXED** (fix #8, `applied_cpu_only`).
- Repeated `(cpu, gpu)` twin-call pairs across dev.c — **FIXED** (fix #8, `side_mark_gap`/`side_set_status`).
- `redis.h` leaks `<hiredis/hiredis.h>` + transparent `redis_client_t` to every includer — maintainability, conf 0.78. Tracked as **WI #53**.
- `TELEMETRY_HOSTS_MAX` / `DEV_HOSTLIST_MAX` both 16, no static assert — **FIXED** (fix #10).

### Testing gaps (advisory, all P3)
- No test for MB values in (INT32_MAX, UINT32_MAX] through dev_graph axis (partly addressed by applied fix #1's non-negative assertions).
- dev_side_resolve with assigned+seen+reachable+!ever_live (never-live host) untested.
- dev_hostlist_merge repaint-only (false) return on online→offline-same-membership untested; cap/drop selection unverified.
- Host-token letter-range boundary chars (`@ [ ` {`) untested; exact-fit output buffer (outsz==hlen+1) accept untested.
- gpu as truthy non-object (`"gpu":5`) untested; gpu gate saturation over long run untested; loss_n==1 / loss_n<=0 untested.

## Advisory / informational

- **Security:** no findings. Host-token contract is a real choke point (charset/anchor/length, honors r->len, embedded-NUL safe), persisted assignments re-validated, AUTH never logged, hiredis `%s` + charset closes command injection. Plaintext LAN transport is an accepted home-lab decision.
- **Agent-native:** two runtime gaps — (1) no live programmatic dev host reassignment (`kdeskdash:dev:left|right` only read once on dev entry; live change is touch-only); (2) no published view/liveness state for an agent to observe what's rendered. Doc gaps addressed by fix #2.
- **Learnings:** docs/solutions/ is empty — no prior art. Good `/ce-compound` candidate (liveness ladder, GPU debounce gate, untrusted cJSON telemetry).
- **Performance:** host-count work hard-capped at 16; charts fixed 300 pts; no unbounded growth; reply/cJSON lifetimes balanced on all paths.
- **Requirements (inferred plan):** R4/R14/R16/R17/R18 (Unit 7) + Units 2–6 all present in diff and hardware-verified. R19 (180° rotation, Unit 1) intentionally deferred/neutralized — now documented accurately by fix #3.

## Coverage

- Suppressed: findings below 0.60 confidence (none material; lowest retained at 0.60).
- Untracked files excluded: none.
- Failed/timed-out reviewers: none (10/10 returned).
- Stack-specific personas (Rails/Python/TS/frontend), api-contract, cli-readiness, data-migrations, previous-comments: not applicable (C embedded, no web API/CLI/migrations/PR).
