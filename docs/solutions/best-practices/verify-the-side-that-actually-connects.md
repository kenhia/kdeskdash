---
title: "\"Both sides are proven\" is only true if you name the two sides in the path"
date: 2026-08-09
category: docs/solutions/best-practices
problem_type: best_practice
module: kvscf feed (kdeskdash reader, kvscf publisher, redis-kvscf instance)
component: integration
severity: high
applies_when:
  - Adding auth to an endpoint a second program already talks to
  - A plan asserts a capability exists because "we already do this elsewhere"
  - A secret has both a transport layer and an application layer
  - Committing a config that must not start up in its insecure default
related_components:
  - deploy/redis-kvscf.conf
  - src/kvscf_redis.c
  - src/config.c
  - docs/kwork-rpidash3-pairing.md
tags: [redis, auth, integration, security, fail-closed, planning]
---

# "Both sides are proven" is only true if you name the two sides in the path

## Context

Sprint 026 put a `requirepass` on rpidash3's kvscf Redis, so that kwork could
publish window/launcher feeds to it across the LAN. The work item said the
security work was cheap and low-risk, and gave its reasoning:

> Both sides are already proven: kdeskdash's client takes an `auth` argument and
> rpi53's instance already requires one.

Both halves of that sentence are true. Neither side it names is in this path.

The endpoint is rpidash3's Redis. The two programs that connect to it are
**kdeskdash's reader** (proven, yes) and **kvscf on kwork** (never mentioned).
rpi53's instance is a *server* requiring auth, which says nothing about whether
any particular *client* can supply it. Reading kvscf's `remote.rs` took two
minutes and showed it building a bare `redis://{host}:{port}` — no password
field anywhere in its config struct.

Had the plan been executed as written, the `requirepass` would have gone on, the
publisher would have entered a reconnect loop, and the symptom — an endpoint
that never produces data — is indistinguishable from an unreachable host.

## The rule

**Name the two programs that open the socket, and check each one's source.**
Not the protocol, not a sibling deployment, not "we do this elsewhere". A
capability claim about an integration is a claim about two specific binaries at
two specific versions.

The tell for this failure is a sentence of the form "both sides already do X"
where the two sides are not enumerated. When you see it — in a work item, a
handoff, your own plan — expand it. Half the time the expansion is fine and
costs a grep. The other half it names a program nobody checked.

## The corollary: transport auth and application auth fail differently

This pairing ended up with two secrets, and confusing them wastes a debugging
session because they present as opposite symptoms:

| Layer | Secret | Checked by | Wrong or missing looks like |
|---|---|---|---|
| Transport | `requirepass` / `KDESKDASH_KVSCF_REDISCLI_AUTH` / `KVSCF_REDIS_PASSWORD` | Redis, at connect | an **unreachable endpoint** — no data at all |
| Application | `KVSCF_TOKEN` | kvscf, per command | **nothing at all** — feeds render fine, only taps silently do nothing |

A wrong application token produces no error on either end by design (the
receiver drops an unauthenticated command; the sender is fire-and-forget). So
"the panel still shows windows" is not evidence the token is right, and any
verification that stops at "data is flowing" has tested exactly one of the two.

## The other corollary: an insecure default must not be startable

The committed conf for the new instance ends with:

```
include /etc/redis/redis-kvscf-local.conf
```

That uncommitted file holds the two things that cannot live in the repo: this
board's LAN `bind` address, and the `requirepass`. Redis **fails to start** when
an `include` target is missing, and that is the reason the split is shaped this
way rather than as a documented "remember to add the password" step.

The failure mode being designed against is not "someone forgets the password".
It is "the service came up, the panel works, and nobody notices the listener is
open" — because a LAN-bound Redis with no auth behaves *better* than a correct
one, right up until it matters. Fail-closed converts a silent misconfiguration
into a loud one.

Verify it by removing the file and watching the unit fail, the same way sprint
010's sandboxing lesson was learned: a safety property that has only ever been
committed is a safety property nobody has tested.
