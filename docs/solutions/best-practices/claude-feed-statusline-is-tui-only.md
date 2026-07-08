---
title: "Claude feed: the statusline runs only in TUI sessions, so enrich model from the transcript in the hook"
date: 2026-07-07
category: docs/solutions/best-practices
module: Claude mode feed (publisher/claude-pub.sh, src/modes/claude.c, src/claude_feed.c)
problem_type: best_practice
component: telemetry_publisher
severity: medium
applies_when:
  - A Claude Code publisher relies on the statusLine command for per-session data
  - Sessions run via the desktop app, headless (claude -p), or the Agent SDK, not just an interactive terminal
  - A dashboard field (model, title, usage limits) has silently stopped refreshing
related_components:
  - claude mode
  - claude-feed Redis (rpidash2:6380)
  - session hash enrichment
tags: [claude-code, hooks, statusline, telemetry, redis, freshness, transcript]
---

## Problem

The `claude` mode showed usage gauges and per-session `model`/`title` that went stale
(days old) even while agents were actively running across the fleet. All three fields were
published only by the **statusline** handler in `claude-pub.sh`.

## Root cause

Claude Code invokes the `statusLine` command **only in an interactive terminal (TUI)
session**. The desktop app and headless runs (`claude -p`, Agent SDK) fire the lifecycle
**hooks** (SessionStart / UserPromptSubmit / Stop / SessionEnd) but never the statusline.
Proven two ways: a headless `claude -p` probe with a logging statusline+hooks logged the
hooks and zero statusline calls; and a machine that only ever ran the desktop app had a
publisher state dir full of hook-written `.start` files but no statusline-written
`limits.stamp` / `.title` files. So on a fleet dominated by desktop/headless sessions,
anything sourced only from the statusline stops refreshing.

## What to do

- **Model / title-style enrichment**: read it from the transcript in the hook. The hook
  payload carries `transcript_path`; the transcript JSONL has `"model":"claude-..."` on
  every assistant line. A bounded `tail | grep | sed` keeps the cost independent of
  transcript size, and it works for every session type. (Normalize the id to a short label
  in the publisher: `claude-opus-4-8` -> `Opus 4.8`.)
- **Usage limits (5h / 7d percentages) genuinely cannot** be recovered from hooks or the
  transcript — they exist only in the statusline `rate_limits` field. Do not fake freshness:
  make the consumer honest instead. `cf_limits_stale()` greys the gauges and badges the
  "as of" line once the snapshot passes `CF_LIMITS_STALE_S`, so a quiet fleet reads as
  stale rather than implying live numbers.

## Gotcha

Converting a Windows `transcript_path` (`C:\Users\...`) to a Git-Bash-usable path
needs a real backslash->slash translation. `tr '\\' '/'` is unambiguous; the `${var//.../}`
parameter-expansion form is easy to mangle into "delete all slashes" when one backslash is
lost in transit.
