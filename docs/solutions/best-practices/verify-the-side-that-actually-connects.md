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

## Postscript: the same mistake, one layer up, in the runbook

The rollout failed anyway, and for a variant of the same error — this time in
this repo's own documentation rather than the work item's.

kvscf's handoff said "**both secrets** resolve `HKCU\Software\kenhia\kvscf`
first, then env". True, and precise: `KVSCF_TOKEN` and `KVSCF_REDIS_PASSWORD`
read the registry. The other three settings — `KVSCF_REDIS_HOST`,
`KVSCF_REDIS_PORT`, `KVSCF_HOST_NAME` — are `std::env::var` only. The runbook
generalized "both secrets" into "config", published a five-row table headed
*prefer the registry*, and it was followed exactly.

The result was worse than three ignored settings, because the two that *were*
read still worked: kvscf fell back to `DEFAULT_HOST` (rpidash2's IP, compiled
in) and offered it the password it had loaded from the registry. rpidash2's
instance has no `requirepass`, so Redis rejected the AUTH and the channel never
came up — publishing nothing, to the wrong board, with a reconnect loop as the
only symptom.

Two things to take from it:

**A config surface is not uniform until you have checked each key.** "Settings
come from X" is the same shape of claim as "both sides support X" — a summary
that reads as a rule. Both were accurate about the cases their author had in
mind and silent about the rest.

**Print the resolved endpoint, and read it.** kvscf logs
`remote channel up — redis://<host>:<port> auth=<bool>` at startup. That one
line contained the whole diagnosis from the first second, and neither the
address nor the flag was checked until the pairing was already declared broken.
When a component has a compiled-in default that is *another live host*, a
misconfiguration does not look like an error — it looks like talking
confidently to the wrong machine.
