# Sprint 048 — Claude usage: plan-aware five-hour gauge, live kai token, cleo's moved file

**Proposal:** korg:4675.
**Covers:** WI 4670, 4672, 4673.
**Branch:** `048-claude-usage-plan-aware`, off `d2e11ee`.

This sprint was started by hand from a kubs0 session. Ken reported that the
Claude usage numbers had stopped updating from Claude Desktop on cleo, and that
they were about 6 hours old before he opened a session. 047 had fixed the
publisher's five-hour fields and shipped "panels unchanged", which left the job
half done. Anthropic dropped the five-hour window from the top plan, which is
now called Ultra (formerly Max 20x). The smaller plans still have one, so the
code has to handle both: anyone using it from GitHub may be on either.

The repo is on kai, so it was edited through kaed (`kai:src`). Builds and tests
ran on kai over ssh.

## Premise check

Every item was checked against live state on 2026-11-14 at about 08:40Z.

- **4672 holds.** `render_limits()` drew the 5 HR gauge unconditionally. The
  vendored `lib/kdashdata` (4837473) predates CD-29 and had no
  `five_hour_none`. The live record read `five_hour_none 1`.
- **4673 holds.** On kai, `.credentials.json` was rewritten at 00:40 PST, when
  karc legs started `claude -p`, and its token expires 8 hours later. The
  timer logged "Finished" every 5 minutes either way. `oauth_token()` returned
  1 on expiry, and the poll exited 0 having written nothing.
- **4670 holds, and it is worse than filed.** On cleo the MSIX LocalCache
  folder now holds only a `.user-data-left` marker. The file is now at
  `%LOCALAPPDATA%\Claude-Data\claude_pzs8sxrjxfjjc\`, with 33 samples hours
  apart and `sd` only. The app has been running since 18:39 and hasn't written
  a sample since.

## Decisions

- **The 5 HR gauge hides only on a positive "no window".**
  `claude_five_hour_visible()` is false only for a *valid* record with
  `five_hour_none`. A record that predates the field, or no record at all,
  keeps the two-gauge layout. The whole column hides, and the row's centre
  flex re-centres 7 DAY. The scoped (Fable) row is unchanged.
- **kdashdata is pinned to 8cb64c6, the commit kpidash pins, and not to
  main.** It carries CD-29. Main has since gained the calendar contract
  (b1ad6af), which a parallel program is landing, and this sprint has no
  reason to pull it in.
- **Expired token: ask the CLI (Ken's call, WI 4673 comment 4695).** First
  `claude auth status`, then the smallest `-p` turn: Haiku, no tools, no MCP,
  no session persistence, `--setting-sources project`, run from `$TMPDIR`. That
  keeps user-level hooks, this publisher's own included, from showing a phantom
  session on the panel. It runs at most once an hour (`oauth-refresh.stamp`).
  A token still dead afterwards writes `state/oauth-expired`; a later
  successful refresh removes it. `--bare` was rejected because it never reads
  OAuth.
- **The history file is no longer a 5-minute feed, so it is no longer
  first-or-nothing.** The order is now: a fresh sample (≤ 900 s), then oauth,
  then any sample regardless of age. An old sample is published with its own
  `t` as `updated_at`, so the panel greys it, and the freshness guard keeps any
  newer writer on top. On cleo, whose CLI credentials are dead, that means an
  honest stale reading in place of nothing at all.

## What shipped

- `src/modes/claude.c`, `src/modes/claude_view.{c,h}`: the plan-aware 5 HR
  column. `tests/test_claude_view.c` covers both plan shapes, no record, an
  invalid record, and NULL.
- `lib/kdashdata` → 8cb64c6.
- `publisher/claude-pub.sh`: adds the `Claude-Data/*/` path, `from_file
  <max-age>`, the three-step source order, `oauth_token` returning 2 on expiry,
  `oauth_refresh`, and `bounded` (macOS ships no `timeout`).
  `publisher/tests/batch-shape.sh` gains sections 16 and 17 (14 assertions).
- `VERSION` 0.28.0 and `publisher/VERSION` 2.6.0.

**Gate:** `just check` 28/28. Each new publisher assertion was seen to fail:
10 fail before the implementation. Separately, each of four planted
regressions turns the suite red: expiry back to `return 1` (5 fails), the
Claude-Data path removed (34), the old-sample fallback removed (2), and
`--setting-sources` dropped (1).

**Live check of the `-p` fallback** (kubs0, CLI 2.1.325): the exact
invocation returned `ok` with exit 0, and no session file appeared in
`kdeskdash-pub/state`.

**Not proven live:** whether `claude auth status` alone refreshes an expired
token. Testing it on purpose would rotate a real refresh token underneath
running sessions. The code checks the token after each step, so if `auth
status` doesn't refresh, the `-p` step does. The first natural expiry on kai
will show which one did it.

## Found in passing — for Ken

The CLI on kubs0 warns: *"this sign-in ends between 2026-11-19 and
2026-11-22 … Renewing it does not move those dates. Run claude auth login
before then."* `claude auth status` on kubs0 shows the same end date. kai's
shows none. No refresh can fix this; it needs an interactive login.

## Follow-ups

- cleo is still unmanaged (WI 4651): 2.6.0 has to be hand-copied there, as
  2.5.0 was.
