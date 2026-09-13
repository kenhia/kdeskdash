---
title: "A fallback outlives the sameness that justified it"
date: 2026-09-01
category: docs/solutions/best-practices
problem_type: best_practice
module: config (claude feed / kvscf endpoint resolution)
component: configuration
severity: high
applies_when:
  - One setting defaults to another setting's value
  - Splitting two things that have shared an endpoint, a credential or a path
  - A per-field fallback chain where the fields are not independent in reality
  - Moving one of two consumers that were co-located "for now"
related_components:
  - src/config.c
  - tests/test_config.c
  - deploy/hosts/rpidash2.env
  - docs/solutions/best-practices/verify-the-side-that-actually-connects.md
tags: [config, redis, auth, fallback, migration, coupling]
---

# A fallback outlives the sameness that justified it

## Context

`KDESKDASH_KVSCF_REDIS_*` defaulted to the Claude-feed values, and the reason
was written down and correct:

> On rpidash2 the kvscf keys live on the same instance as the claude feed, so
> unset means "reuse the claude-feed values" and that device's env needs no
> change. Each field falls back independently — set only the host and you
> inherit the claude port and auth.

Two feeds, one Redis, one set of settings. For a year that was true.

Sprint 031 moved the Claude feed to the central Redis and left kvscf where it
was, and the work item said exactly what to do about it: pin the kvscf endpoint
explicitly so it does not follow. That was done — host and port, in the same
change, on the one board that was still inheriting.

The panel then read **`kvscf feed unavailable`**, with the `kvscf:*` keys
sitting on the instance it was pinned to.

## What actually happened

The third field. `KDESKDASH_KVSCF_REDISCLI_AUTH` was unset — and had always been
unset, *correctly*, because the Claude handle had never had a password to
inherit. Repointing the Claude feed at an authenticated Redis gave that handle a
password for the first time, and the independent per-field fallback handed the
same password to kvscf.

`127.0.0.1:6380` has no `requirepass`. A Redis with no password configured does
not ignore `AUTH`; it answers with an **error**
(`ERR AUTH <password> called without any password configured`). The handle never
connected, and the panel reported it the way it reports any dead endpoint.

And there was no way to say otherwise. Empty means unset, and unset means
inherit:

(The variable was renamed to `KVSCF_REDISCLI_AUTH` in sprint 037 when the
fleet took over the key names; the code below is quoted as it stood.)

```c
const char *kauth = getenv("KDESKDASH_KVSCF_REDISCLI_AUTH");
cfg->kvscf_redis_auth = (kauth && kauth[0] != '\0') ? kauth
                                                    : cfg->claude_redis_auth;
```

No value of that variable expresses "this one takes none".

## The rule

**A fallback is an assertion that two things are the same. When they stop being
the same, every field that inherits is wrong — not just the ones you remembered
to pin.**

"Each field falls back independently" reads like flexibility. It is really a
claim that the fields are independent *in reality*, and for an endpoint they
never are: host, port and credential are one fact about one server. Splitting
them into three independent defaults means a caller can pin two, believe the
split is done, and silently keep the third.

So: **make the fallback follow the thing that justified it, not the variable.**

```c
bool same_instance = cfg->kvscf_redis_port == cfg->claude_redis_port &&
                     strcmp(cfg->kvscf_redis_host, cfg->claude_redis_host) == 0;
cfg->kvscf_redis_auth = (kauth && kauth[0] != '\0')
                            ? kauth
                            : (same_instance ? cfg->claude_redis_auth : NULL);
```

Now the fallback fires under exactly the condition its comment always claimed:
same instance. Pinning the endpoint is sufficient to pin the whole endpoint,
which is what "pin the endpoint" meant to whoever wrote the work item.

## How to catch it next time

- **When a fallback chain describes a credential, an endpoint or a path, ask
  what makes the two things the same — then make the code test *that*,** rather
  than testing whether someone remembered to set a variable.
- **Grep for what else defaults to the thing you are moving,** before moving it.
  One `grep KDESKDASH_CLAUDE src/config.c` would have shown three inheritors
  where the plan had counted two.
- **A credential fallback's safe direction is "send nothing".** Sending a
  password that is not wanted fails; sending none to a server that wants one
  fails too, but it fails as `NOAUTH`, which names itself. Given a choice, fail
  the legible way.
- **Test the migration, not the steady state.** `tests/test_config.c` pins five
  cases, and the one that would have caught this is the *only* one that has both
  endpoints differing and no explicit kvscf password — the shape that did not
  exist on any device until the day of the move.

## Relation to `verify-the-side-that-actually-connects`

The same failure, one layer down. That note is about enumerating the two
*programs* in the path. This one is about enumerating the two *settings* in the
path: sprint 031 verified that kvscf's endpoint was pinned, and the thing that
had to authenticate was a field nobody had listed as changing.

## A second instance, one variable to the left

Sprint 037 hit the same *outcome* — a handle silently authenticating against a
Redis that has no password — by a different route: not a fallback we wrote, but
a fleet-wide file claiming a generic name the panel already read. See
[a generic env var name is a namespace you
joined](a-generic-env-name-is-a-namespace-you-joined.md). The shared symptom is
worth memorising on its own: **`kvscf feed unavailable` / a dead-looking
endpoint is what a *wrong or unwanted* password looks like on this panel**,
because a Redis with none configured answers AUTH with an error rather than
ignoring it.
