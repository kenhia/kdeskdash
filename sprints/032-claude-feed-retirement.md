# 032 — Claude-feed retirement: one home, no sockets

korg: kdashdata proposal 1754 / work item 1750 — this is the kdeskdash half of
the CD-7 close-out, the last slice of program 1755. No korg work item of its
own: the change is one step in a sequence another repo's sprint owns, and
splitting it into a kdeskdash sprint of its own would have been executing half
a procedure (the precedent the overseer set on korg:1753 for the mid-ship
k-homelab commit).

## What changed

Sprint 031 cut this publisher over to `kdash-pub` and opened a dual-write
window: every write went to the interim home (`rpidash2:6380`, unauthenticated)
*and* the central Redis, while the readers moved. kdashdata sprint 005 retires
the interim home, so:

- **`KDD_LEGS` defaults to `claude`** — one authenticated leg on
  `KDASH_CLAUDE_REDIS`, which names `rpi53:6379` once the stem flips. The
  `interim` leg is deleted outright rather than left as a name that still
  works.
- **`--no-auth` is gone from this script.** It existed for exactly one endpoint,
  and that endpoint no longer carries this feed.
- **The last hand-rolled socket is gone.** Poll mode's freshness guard — don't
  publish over a fresher observation from a live statusline on any host — was a
  raw `/dev/tcp` RESP request, because `kdash-pub` had no read verb and bash
  could not reach an authenticated Redis without re-deriving CD-12's auth rules.
  kdashdata built the verb (CD-14, korg:1769), so the guard is now
  `kdash-pub hget claude:limits <field>`.

## Two things worth keeping

**The guard reads the leg it writes.** `leg_stem` is one mapping used by both
`send_leg` and `stored_epoch`, so the read and the write it guards cannot
resolve to different Redises — a guard comparing against a Redis other than the
one it is about to overwrite is worse than no guard at all. It sets a variable
rather than printing one, because `send_leg` is on the every-tool-call path and
a command substitution there is a fork.

**A retired leg name must publish nowhere.** `interim` is not aliased to a live
home. A stale caller — an un-upgraded unit, an old env file — naming it writes
nothing at all, which is the only behaviour that cannot silently resurrect the
dual-write this sprint exists to end. That is now an assertion, not a comment.

## Gates

`just check` — 19/19, including `test_publisher_batch`. `batch-shape.sh` gained
three assertions and lost the two that pinned the dual-write:

- the **shipped default** is one authenticated leg on the claude stem (run with
  `KDD_LEGS` unset, since a default is the one thing a test that supplies the
  value cannot check);
- the retired `interim` name publishes nowhere;
- poll guards its write with an `hget` **on the same stem it publishes to**;
- and no `/dev/tcp` remains anywhere in the script — the path around khlenv,
  AUTH and the key grammar, and how the last one survived a cutover.

All three new assertions were watched failing before being trusted: reverting
the default to a two-leg value, sneaking a `/dev/tcp` back into `stored_epoch`,
and hardcoding the guard's stem to `central` each failed the expected assertion
and only that one.

One fix to the harness fell out: the `kdash-pub` stub drained stdin
unconditionally, which hangs forever on `hget`. The real CLI reads stdin only
for `batch`, and the stub now does the same.

## Deployed

Recorded in kdashdata `sprints/005-relocation-close-out.md`, which owns the
whole sequence — publish, the k-homelab pin bump and stem flip, both panels
verified, and the `DEL` on the old home. Splitting that evidence across two
repos would leave neither able to answer "did the cutover finish".
