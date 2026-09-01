# Sprint 031 — claude-feed relocation: publisher cutover + reader repoint

korg: proposal 1753, WIs 1748/1749. Slice 2 of the claude-feed relocation
program (korg:1755, kdashdata CD-7). Goal: the `claude:*` feed stops living on
`rpidash2:6380` and starts living on the central Redis — publishers dual-write
through a window so the panels never blank, then both panels read central, with
the kvscf endpoint explicitly pinned so it does **not** come along (CD-8).

Overseen sprint: an overseer session on cleo watches program 1755. The wrap-up
returns as a handoff on korg:1753, and the ship waits for a green light there.

## Premise check, before anything was built

Every falsifiable claim in both items, verified live from kai. All held; two
were narrower or friendlier than written.

- **#1749 is a one-device edit, not two.** `deploy/hosts/rpidash3.env` has pinned
  `KDESKDASH_KVSCF_REDIS_HOST/PORT` since sprint 026. Only rpidash2 was still
  inheriting the kvscf endpoint from the claude one, so only rpidash2 could be
  dragged to central by the repoint.
- **"distribute REDISCLI_AUTH to every publisher host" (#1748) had already been
  solved.** CD-12's file route works: `kdash-pub endpoint` authenticates against
  central with `REDISCLI_AUTH` unset in the shell.
- The endpoints behaved exactly as slice 1 measured: `KDASH_CENTRAL_REDIS` →
  `rpi53:6379` (authenticated), `KDASH_CLAUDE_REDIS` → `rpidash2:6380` where
  `--no-auth` exits 0 and sending AUTH exits 2, `Password authentication failed`.

No cross-project plan applies (kdeskdash is not in
`cross-project-planning/index.md`).

## The gap that parked the sprint for a day

`kdash-pub` had no distribution. It existed only as
`publishers/rust/target/release/kdash-pub` in the kdashdata checkout on kai —
no store publish, no Windows build — so "roll out host-by-host with the panel as
the live check" could not start on kai, kubs0 **and** cleo.

That became slice 1.5 of the program (korg:1764, kdashdata sprint 004). It
shipped `kdash-pub 0.1.0-27bf527` to the package store and installed it at
`/usr/local/bin/kdash-pub` on kai and kubs0 and `C:\tools\bin\kdash-pub.exe` on
cleo, with `endpoint` verified green on all three. Those paths are now a
**contract** (kdashdata CD-13), which changed one of this sprint's decisions
before a line was written — see below.

## Decisions

**The hook wiring does not change.** `claude-pub.sh` keeps its name, path and
three modes and execs `kdash-pub` internally. k-homelab's `claude-hooks` recipe
and cleo's `settings.json` stay out of the blast radius entirely; the whole
cutover is one file in the publisher bundle.

**Exec the fleet path, never a per-user copy.** The brief originally proposed
vendoring the binary next to the script at `~/.claude/kdeskdash-pub/kdash-pub`,
because a hook context's `PATH` is not the interactive shell's. The overseer
revised it: an absolute path answers that concern just as well, and a private
copy would bypass the store — a `knarr deploy kdash-pub` upgrade would never
reach the hooks, which is the kpolice-002 stale-binary drift in per-user form.
So the script resolves `/usr/local/bin/kdash-pub` then
`/c/tools/bin/kdash-pub.exe`, and `KDD_PUB_BIN` overrides for dev.

**The resolved binary is checked for executability however it was chosen.**
Found while writing the test: an override naming a missing file would have
failed at exec time with output already redirected to `/dev/null` — publishing
nothing, and saying nothing about it. `[ -x ]` on the final value closes that,
and both routes now land in the same breadcrumb.

**A host with no `kdash-pub` leaves a breadcrumb, not a message.** A hook must
not print, so the failure the store install exists to prevent writes
`$STATE_DIR/no-kdash-pub` instead. Silence with a file beats silence.

**Three named legs, and the end state is one of them.** `KDD_LEGS` defaults to
`interim,central`, which *is* the dual-write window:

| leg | argv | home |
|---|---|---|
| `interim` | `--stem KDASH_CLAUDE_REDIS --no-auth` | rpidash2:6380, while the stem still names it |
| `central` | `--stem KDASH_CENTRAL_REDIS` | rpi53:6379, authenticated |
| `claude` | `--stem KDASH_CLAUDE_REDIS` | the end state, once slice 3 flips the stem |

Naming the end state now means slice 3's publisher change is `KDD_LEGS=claude`
plus deleting the interim arm, not a re-derivation. An unknown leg name
publishes nowhere rather than guessing at a home.

**The DEL ships in its own batch at SessionEnd.** This is the one real hazard the
port introduces. RESP pipelining made every command independent; `kdash-pub
batch` refuses the **whole** batch when any line is off-contract. The
`claude:recent` record is hand-built JSON, so a record that somehow failed to
parse would have taken the `DEL` down with it and left a ghost session until the
2h TTL. Two batches, DEL first, and the ghost cannot happen.

**Legs run in parallel and `cmd()` never forks.** `send_sync` backgrounds every
leg and waits, so two homes cost one home's latency. `cmd()` strips the
delimiters with bash parameter expansion rather than `tr`, because it runs on
every tool call in every session on the box and 24 forks per hook is not free.

## What shipped

- **[`publisher/claude-pub.sh`](../publisher/claude-pub.sh)** — the RESP builder
  and the `/dev/tcp` sender are gone from the write path; commands accumulate as
  kdash-pub's tab-separated `batch` format and go out one connection per leg. No
  host, port or IP is written down in the script any more.
- **[`publisher/tests/batch-shape.sh`](../publisher/tests/batch-shape.sh)** —
  eight assertions on exactly what the script hands the CLI, registered as
  `test_publisher_batch` in `just check`. It stubs `kdash-pub`, so it needs no
  network and no installed binary.
- **`publisher/VERSION` → 2.0.0** — the base bump is deliberate: a host can no
  longer install this bundle by dropping the script alone, which is a breaking
  change to the install contract even though the panel-facing key contract is
  byte-identical.
- **The reader repoint** — `deploy/hosts/rpidash2.env` and `rpidash3.env` point
  the claude handle at `rpi53:6379`, and rpidash2 gains the explicit
  `KDESKDASH_KVSCF_REDIS_HOST/PORT` pin CD-8 requires.
- **Docs** — `publisher/README.md` (the CLI dependency, the leg table, a
  both-homes smoke test), `deploy/hosts/README.md` (the fourth secret),
  `deploy/kdeskdash.env.example` (the kvscf fallback is now legacy, not a
  default) and the top-level README.

## The bug the repoint surfaced: an auth fallback that outlived its reason

Pinning rpidash2's kvscf endpoint to `127.0.0.1:6380` was not enough, and the
panel said so: **`kvscf feed unavailable`**, with the keys sitting right there on
that instance.

`config.c` let each kvscf field fall back to the claude one *independently*.
Host and port were pinned; auth was not, because `KDESKDASH_KVSCF_REDISCLI_AUTH`
was unset — and it had always been unset, correctly, because the claude handle
had never had a password. Giving the claude handle a password for the first time
handed kvscf one too, and rpidash2:6380 has none configured, which makes AUTH an
**error** rather than a no-op. The handle stopped connecting.

No value of the variable could fix it: empty means unset, which means inherit.
There was no way to say "this one takes none".

The fallback now follows the **endpoint**, not the variable: the claude password
is inherited only when the kvscf endpoint resolves to the same `host:port`,
which is the exact condition the fallback was written for ("both live on the
same Redis"). Once they differ, inheriting is never right. `tests/test_config.c`
drives the real `config_load()` through `setenv` and pins all five cases,
including rpidash3's — an explicit kvscf password still wins over everything.

This is `verify-the-side-that-actually-connects.md` again, one layer down: the
plan verified that kvscf's *endpoint* was pinned, and the thing that had to
authenticate was a field nobody had listed as changing. Written up as
[a-fallback-outlives-the-sameness-that-justified-it.md](../docs/solutions/best-practices/a-fallback-outlives-the-sameness-that-justified-it.md).

## Rolled out and verified live

Branch-proving builds, published to the store and installed from it — the same
pattern slice 1.5 used. `latest` moves at merge.

**Publishers — `kdeskdash-publisher 2.0.0-d619f85`, all three hosts, one SHA
(`98623b90…`), verified by naming them rather than by iterating what the runner
reached:**

| host | install | probe landed on interim | on central |
|---|---|---|---|
| kai | `~/.claude/kdeskdash-pub/` | ✓ | ✓ |
| kubs0 | same, SHA-checked against the store | ✓ | ✓ |
| cleo | same, via a PowerShell fetch + SHA check | ✓ | ✓ |

The previous copy is kept as `claude-pub.sh.prev` on each host, and `VERSION`
alongside it says what is installed.

Then, unprompted and better than a probe: **this very sprint's own Claude session
started dual-writing mid-flight.** Its heartbeat wrote `ts=1788242177` to both
homes, byte-identical, with no restart of anything.

**Readers — `kdeskdash 0.27.0-6817457` on both boards.** Device env files were
hand-edited (`install-service` never overwrites an existing one), with
`.pre031` backups left in place. The claude password was derived from the
telemetry one **on the device** — same rpi53 secret, so nothing crossed the wire
or a transcript.

- **rpidash2** — claude mode reads central: the USAGE gauges match what central
  holds, and the AGENTS pane correctly showed nothing while central had no
  session hashes and interim had three. That absence *is* the proof of which
  home it is reading. Launcher renders a live cleo button again, over a socket
  to `127.0.0.1:6380`.
- **rpidash3** — repointed for hygiene; it registers no `claude` mode, so that
  handle is never initialised there and there is no claude panel to check. Its
  kvscf tap is the live check: the handle connects to `127.0.0.1:6380` (its own
  password, unaffected by the auth rule change) and the panel reads "no launcher
  configured" because kwork is asleep and the instance is genuinely empty — the
  documented empty state, not a fault.

**Both panels' `KDESKDASH_TELEMETRY_REDISCLI_AUTH` copies verified against
rpi53** (`PING` → `PONG`, `EXISTS claude:limits` → 1) from the devices
themselves. klams listed them as unverified since the August rotation; they are
correct.

**kai's poll timer dual-writes.** Its 22:57 firing left
`claude:limits updated_at=1788242234` on both homes, identical.

### One property of the window worth knowing before the next one

A session that was **already running** when its publisher cut over produces a
*partial* hash in the new home. The keepalive deliberately writes only `ts` — it
can make a row fresher but must never change what it claims — so the new home's
key has no `status` or `host`, and `cf_session_from_fields` correctly declines
to render it. The panel under-reports until each session takes its next turn,
at which point `UserPromptSubmit`/`Stop` writes the full field set and it heals.

Not a bug and not worth engineering around (a keepalive cannot invent a status),
but it is why the claude panel can look emptier than reality for a few minutes
after a cutover, and it is the reason to start a window when few sessions are
live rather than mid-afternoon.

## The one hand-rolled socket left, and why

Poll mode still reads `claude:limits` over `/dev/tcp` — `kdash-pub` has seven
write verbs and no read verb. The read is the guard that stops a poll writer
publishing over a **fresher observation** from a live statusline on any host.

It reads the interim home, which takes no AUTH and, while every publisher
dual-writes, carries every observation central carries; failure is benign, since
an empty read means "unknown" and the guard then publishes. The endpoint is
resolved from the same stem the write path uses, so even this socket has no
address written down.

That stops being true at the slice-3 stem flip, and **korg:1769** is filed for
it (a `kdash-pub hget`), with `depends_on` from slice 3.
