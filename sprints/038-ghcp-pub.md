# 038 — ghcp-pub: the Copilot CLI hooks publisher

korg:2755, slice 4 of program korg:2751 (kxeneon Agents panel). Covers WI 2753.
Run as karc leg `kdeskdash-09cdc9` on kai, under `/overseen-sprint`.

## Goal

A `publisher/ghcp-pub.sh` beside `claude-pub.sh` that publishes GitHub Copilot
CLI session activity to `ghcp:session:{host}:{sid}`, so kxeneon's Agents panel
shows Copilot sessions beside Claude ones. Same `kdash-pub` transport, same
stem, same TTL and keepalive. Shipped in the existing kdeskdash-publisher store
bundle with a version bump; k-homelab's `copilot-hooks` recipe (slice 5,
korg:2756) installs it.

The contract landed in slice 3: kdashdata `ghcp-session.schema.json` (CD-21),
merged to kdashdata main as `5203e29`.

## Premise check

| Claim | Verdict |
|---|---|
| `claude-pub.sh` has the shape to mirror (absolute `kdash-pub`, batch, `--stem KDASH_CLAUDE_REDIS`, 2 h TTL, throttled keepalive, exit 0 always) | **holds** |
| The store bundle exists and `publish-publisher.sh` stages it | **holds** — though it stages `VERSION` and the payload, not a `SHA256SUMS`; the WI said otherwise. Points the same way, no consequence. |
| The batch-shape test pattern (`KDD_STATE_DIR` + a stub `KDD_PUB_BIN`) is reusable | **holds** |
| `ghcp:*` rides `KDASH_CLAUDE_REDIS` | **holds** — the registry's own endpoint row lists `claude:*, ghcp:*` |
| Copilot hooks live in `~/.copilot/hooks/<name>.json`, `version: 1` | **holds** |
| Copilot CLI on kai is 1.0.83 (what slice 3 measured) | **drifted** — kai now runs **1.0.85**. Re-measured rather than inherited; every slice-3 finding reproduced unchanged. |

## First act: measurement, not code

Slice 3 measured Copilot 1.0.83 and left one question open, which the overseer
put on this proposal as the sprint's first task: the probe had run
`--allow-all-tools`, so the permission path was never exercised and `blocked`
was recorded as *unobserved*, not absent.

A probe hook declaring 20 event names (the six documented, plus 14 speculative
permission/turn-end names) dumped each event's stdin. Removed afterwards; the
pre-existing unmanaged `klams-sync.json` verified byte-identical and
mtime-unmoved before and after.

### Reproduced on 1.0.85

- The complete user-level set is `sessionStart`, `sessionEnd`,
  `userPromptSubmitted`, `preToolUse`, `postToolUse`, `errorOccurred`. None of
  the 14 speculative names fires.
- **No turn-end event.**
- `timestamp` is **milliseconds** (`1789622590920`).
- `userPromptSubmitted` fires **~6 ms before `sessionStart`** in `-p` mode.
- Every payload carries `sessionId`, `timestamp`, `cwd`; sessionStart adds
  `source`/`initialPrompt`, tool events add `toolName`/`toolArgs`, postToolUse
  adds `toolResult`, sessionEnd adds `reason`.
- No model, no title on any event.

### New: `blocked` has no source, and now we know why

`--allow-all-tools` is documented as *required for non-interactive mode*, so a
`-p` session cannot sit at an approval dialog — it is refused outright. Running
one anyway, with a write and an out-of-scope read, produced the decisive shape:

**three `preToolUse` events and zero `postToolUse`.** A refused tool fires pre
and never post.

That tells us `preToolUse` fires **before the permission decision**, not after
it — so in interactive mode it is the last event before the session sits on the
dialog. It is also the event before every auto-approved tool, and it carries
nothing separating the two.

So the tempting move is unsound: `claude-pub.sh` publishes `blocked` on
`AskUserQuestion` because that is one specific tool whose whole purpose is to
block on the user. Copilot's block is a property of the **permission system**,
not of a tool. Emitting `blocked` on `preToolUse` and `working` on
`postToolUse` would mark every long-running build as blocked — which is the
busiest a session ever is.

**The schema's "never emits blocked" stands.** Recorded in the script header
and the README.

Two attempts to drive a real interactive dialog on a pty failed — the TUI would
not accept a synthetic prompt headless — so the dialog itself is still
unobserved directly. It does not change the conclusion, which rests on the
pre-before-decision ordering rather than on watching the dialog. Said plainly
rather than papered over.

### New: `sessionEnd.reason` has more than one value

`complete` from a `-p` run, `user_exit` from an interactive quit. Both fixtures
are in the test. Also: `sessionEnd` can fire for a session that never fired
`sessionStart` (an interactive session killed before submitting a prompt) — a
DEL on a key that was never written, which is harmless.

### New, and deliberately not acted on: model and title *do* exist on disk

WI 2753 asked the sprint to find out whether a session title exists. It does:

- `~/.copilot/session-state/<sid>/events.jsonl` → `session.start` carries
  `selectedModel` (e.g. `gpt-5.6-sol`) and `copilotVersion`.
- `~/.copilot/session-state/<sid>/workspace.yaml` carries `name` — **and a
  `user_named` flag**.

Not wired, for two reasons. The overseer's carried-forward instruction was
explicit ("no event carries a model or a title — do not try"), and this is a
contract-prose question for the owning repo rather than a call to make from
here. And the second reason is the one that matters even if the first is lifted:
when `user_named` is `false` — the default — that "name" **is the user's raw
initial prompt**, so publishing it would put prompt text into Redis and break
the rule `claude-pub.sh` keeps (R20). Anyone wiring this later must gate on
`user_named`. Flagged to the overseer.

Incidentally, `events.jsonl` *does* carry `assistant.turn_start` /
`assistant.turn_end`. The turn boundary exists inside Copilot; it is simply not
exposed as a hook. The schema already anticipates gaining `awaiting` with no
change if that ever ships.

## What shipped

- **`publisher/ghcp-pub.sh`** — the publisher.
- **`publisher/ghcp-hooks.json`** — the user-level hook template, all five
  wired events, `bash` and `powershell` variants.
- **`publisher/tests/ghcp-batch-shape.sh`** + **`tests/fixtures/ghcp/`** — 22
  assertions against payloads captured verbatim from live 1.0.85 sessions.
- **`CMakeLists.txt`** — registers `test_publisher_ghcp`.
- **`scripts/publish-publisher.sh`** — both new files join the bundle payload.
- **`publisher/VERSION`** → `2.2.0`.
- **`publisher/README.md`** — a section for the new publisher.

### Decisions

**The event name is an argument.** Claude Code puts `hook_event_name` in the
payload; Copilot puts the event name nowhere at all. The hook declaration is
the only thing that knows which event fired, so it passes the name as `argv[1]`.
No mode word: there is one mode, and a constant argument is ceremony.

**One bundle, one version clock.** `ghcp-pub.sh` joins the existing
kdeskdash-publisher artifact rather than getting its own: same feed, same
transport, same hosts, same recipe. A second clock would only make "which
publisher is on this host" a two-part question.

**No `ghcp:recent`.** The registry keeps it optional and *unschema'd*. Inventing
it would mint a contract this repo does not own, so `sessionEnd` is a bare DEL.

**`errorOccurred` is not wired.** Real, measured, and there is no status this
feed could honestly publish from it.

**No comment keys in the hook template.** `settings-fragment.json` carries
`"//"` notes because Claude Code tolerates them; Copilot is not known to, and
the failure mode is the bad one — an unparseable hook file disables every hook
in it *silently*, exactly like a mistyped event name. The README carries the
notes instead.

**Milliseconds are converted by the schema's bound, not by counting digits.**
`ts_of()` divides while the value exceeds `1e11`, so it stays correct if Copilot
ever changes the unit, and falls back to the local clock rather than emitting a
stamp the schema rejects silently.

**The JSON helpers take the FIRST match, not the last.** `claude-pub.sh` uses a
greedy `sed`; here `toolArgs` holds a tool's own arguments, so its keys are
outside our control and a tool taking an argument called `timestamp` or `cwd`
is ordinary. Copilot emits top-level fields first, so leftmost-match reads the
payload and never its cargo.

### The tests have teeth (checked, not assumed)

Two mutations, each reverted:

1. Remove the millisecond conversion → **7 assertions fail**.
2. Make both JSON helpers take the last match → **2 assertions fail**.

Mutation 2 initially passed, which exposed a bad test of my own: the decoy was
a `"timestamp":0` inside a JSON *string*, and a JSON string cannot contain a
bare `"timestamp":0` (the quotes must be escaped), so it never matched anything.
Replaced with a nested **object key** — the real hazard — after which the
mutation is caught. Noted in the test, because the wrong version looked right.

## Live acceptance: blocked, and the blocker is real

`just check` is green (22/22). The live half of WI 2753's acceptance — a real
`ghcp:session:kai:<sid>` on rpi53 — **could not be met, for a reason outside
this repo.**

Installed the publisher and hook on kai, ran a real Copilot session with a
pinned `--session-id`, and polled for 60 s. The key never appeared. Every write
was refused:

```
kdash-pub: namespace "ghcp" is not one of kdash, kpidash, claude, kvscf,
kdeskdash, kstudiodash — a feed with no schema in kdashdata is off-contract
```

Diagnosed with a control rather than inferred — same batch, same stem, same
host:

| key | rc |
|---|---|
| `ghcp:session:kai:<sid>` | **1** (refused) |
| `claude:session:kai:probe` | **0** (accepted, then deleted) |

So the endpoint, auth and transport from kai are all fine; only the namespace
allowlist refuses. The hooks did fire and the script did run correctly — the
state directory was created during the session and its throttle file cleaned up
by `sessionEnd`, exactly as designed.

**And it is not merely a stale binary.** No kdashdata source knows `ghcp` at
all: `publishers/rust/src/keys.rs` and `publishers/python/src/kdash_pub/keys.py`
both stop at `kstudiodash`. Slice 3 landed the schema, the registry row and the
rules.md exception — the *documents* — and nothing taught the publisher CLI the
namespace it had just legalised.

This is this repo's own `verify-the-side-that-actually-connects` lesson, landing
one slice downstream of where it was written: the contract declares `ghcp:*`
legal and the enforcement refuses it, and the two were never checked against
each other.

It blocks slice 5 (korg:2756, the k-homelab recipe) and slice 7's Copilot rows
as well as this slice's acceptance, so it is program-level.

**Not repaired here, deliberately.** The code change is two lines, but shipping
it means a new `kdash-pub` **store artifact** and a fleet redeploy of a shared
binary that six feeds route through — §4's Branch B names a store artifact
explicitly, and §3b's guardrail says a fix that changes behaviour someone else
depends on is a decision, not a repair. Filed as a work item naming that
decision; raised on the proposal for the overseer's ruling.

kai was left clean afterwards: hook and publisher copy removed (slice 5 owns the
install), `klams-sync.json` byte-identical and mtime unmoved.

**Probed from kai** — the host the publisher runs on and the host the work ran
on.

## Status

Implementation complete and gated; live acceptance blocked on kdashdata.
Awaiting the overseer's ruling on whether to ship on unit evidence and verify
live once `kdash-pub` learns the namespace, or hold.
