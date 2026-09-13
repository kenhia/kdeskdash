# A generic env var name is a namespace you joined, not a name you own

**Sprint 037.** kdeskdash read `REDISCLI_AUTH` for its control Redis — the
obvious name, the one `redis-cli` itself uses, and correct in isolation for
four years' worth of sprints. Then the fleet defined `REDISCLI_AUTH` to mean
"the central rpi53 password" and started rendering it into
`/etc/khomelab/secrets.env` on every host.

The moment this panel's unit gained `EnvironmentFile=-/etc/khomelab/secrets.env`,
the control handle would have begun sending AUTH to this board's own
passwordless local instance. Measured on both panels:

```
$ REDISCLI_AUTH=anything redis-cli -p 6379 ping
AUTH failed: ERR AUTH <password> called without any password configured
```

Remote mode control, last-mode persistence, GoL settings injection and the
screenshot trigger would all have stopped — silently, because the panel keeps
drawing and nothing on screen depends on that handle.

## What makes this its own trap

It is [a fallback outlives the sameness that justified
it](a-fallback-outlives-the-sameness-that-justified-it.md) with the mechanism
inverted, and that is why reading that page would not have saved you.

There, a *fallback we wrote* kept asserting a sameness that had stopped being
true. Here there is no fallback and no code change at all: **the value arrives
from outside, under a name we did not reserve, into a program that was already
correct.** The diff that breaks it is one `EnvironmentFile=` line in a unit —
and the variable it breaks is not mentioned anywhere in that diff.

Grepping the change for `REDISCLI_AUTH` finds nothing. Grepping the *codebase*
finds it, which is the only reason this was caught before it shipped.

## The rule

**Before adopting a shared configuration source, enumerate every name it will
inject and grep your own code for each one.** Not the names you are adding —
the names the *source* carries. A shared file, a parent process's environment,
a merged config layer: each is a namespace you are joining, and anything in it
that collides with a name you already read is a silent redefinition of your own
program's behaviour.

Corollaries:

- **Generic names are the dangerous ones.** `REDISCLI_AUTH`, `DATABASE_URL`,
  `LOG_LEVEL` — a name chosen because it is conventional is a name someone else
  will also choose conventionally. A name nobody else would pick
  (`KDESKDASH_CONTROL_REDISCLI_AUTH`) cannot be claimed underneath you.
- **The direction of the fix is to rename *yours*, not to refuse theirs.** The
  fleet's meaning is the more general one and has eight hosts behind it; the
  local meaning is the one that should carry a qualifier.
- **Test the absence, not just the presence.** The regression test here asserts
  that setting `REDISCLI_AUTH` leaves the control handle unauthenticated —
  a test that would have failed on the pre-037 code and passes now. Asserting
  the *new* name works would not have caught it.
- **A shared file's other keys are not inert just because you ignore them.**
  They are inert only where nothing you run reads that name — which is a fact
  about your whole dependency tree, not about your intent.

## Where the pattern shows up next

Any consumer joining `/etc/khomelab/secrets.env`. The fleet list is
`REDISCLI_AUTH`, `CLAUDE_REDISCLI_AUTH`, `KVSCF_REDISCLI_AUTH`,
`POSTGRES_PASSWORD`, `KORG_DB_PASSWORD`, `GRAFANA_ADMIN_PASSWORD`,
`UNIFI_CONTROLLER_USER`, `UNIFI_CONTROLLER_PASSWORD`, `HF_TOKEN` — and every one
of those is a name a program might plausibly already be reading for its own
local purpose. `POSTGRES_PASSWORD` is the next most likely collision on the
fleet, since it is what the official Postgres image documents.
