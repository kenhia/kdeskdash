---
title: "Adopting a shared library inherits its defaults — and the one that bites is auth"
date: 2026-09-10
category: docs/solutions/best-practices
problem_type: best_practice
module: Claude mode feed (src/modes/claude.c, lib/kdashdata)
component: integration
severity: high
applies_when:
  - Replacing a hand-rolled client with a shared library that talks to the same endpoint
  - The old and new clients both accept a "password" parameter
  - A refactor is expected to change no behaviour at all
related_components:
  - claude mode
  - libkdash (kdashdata)
---

## The trap

Sprint 034 replaced kdeskdash's own `claude_redis.c` with libkdash's readers.
Both clients take a nullable auth string, and both were being handed
`cfg->claude_redis_auth`, which this project sets to `NULL` when the env var is
unset. Same parameter, same value, same endpoint — and opposite meanings:

| `auth` | retired `claude_redis.c` | libkdash `kdash_conn_opts_t` |
|---|---|---|
| `NULL` | send no AUTH | **read `$REDISCLI_AUTH`** |
| `""` | send no AUTH | send no AUTH |

A bare `REDISCLI_AUTH` is a documented option in `deploy/kdeskdash.env.example`,
and the unit loads `/etc/kdeskdash/secrets.env`, so it can legitimately be in a
panel's environment. Passing `NULL` straight through would have had a board
quietly start authenticating against the claude feed — with the *control*
Redis's password — as a side effect of a change whose whole claim was that
nothing would change.

`claude.c` therefore passes `redis_auth ? redis_auth : ""`.

## Why it is hard to catch

Nothing about the port *looks* like it touches authentication. The diff is a
reader swap; the parameter name is identical on both sides; the types match, so
the compiler is silent. And it fails in the direction that reads worst — a
Redis with no password configured answers `AUTH` with an **error**, not a
shrug, so the handle never connects and the panel reports the endpoint as down.
You would go looking at the network.

## The general rule

**A library's "unset" is not your "unset".** When you replace a client with a
shared one, read the new type's field documentation for every parameter you are
forwarding, not just the ones you are changing — especially where a sentinel
(`NULL`, `0`, `""`, `-1`) carries meaning. Forwarding a value unchanged is not
the same as preserving behaviour.

Two siblings of this rule, both already in this directory:

- `verify-the-side-that-actually-connects.md` — transport auth and application
  auth fail in opposite ways, so "data is flowing" tests only one of them.
- The same sprint's other inherited sentinel: `kdash_claude_sessions()` is a
  **counted reader**, where a negative return means "the read did not complete"
  and neither `out` nor `*skipped` carries information. Treating it as a count
  renders a partial list as a complete one — a confident wrong answer.

Both are the same shape: the value crossed the boundary intact and its
*meaning* did not.
