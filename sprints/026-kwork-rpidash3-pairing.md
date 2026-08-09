# 026 — kwork/rpidash3 pairing: isolated Redis, Launcher on both desks

WI #1137 · proposal korg:1142 · slice 5 (last) of program korg:1143 "Launcher —
retire the Stream Deck". Also closes kvscf WI #1144 (token rotation).

## Goal

Stand up the work-desk half of the Stream Deck replacement: a second, isolated
Redis on rpidash3 that kwork publishes to, `launcher` turned on for both panels,
and the physical Stream Deck unplugged.

No C changed. This is a config, infrastructure and documentation sprint — both
boards were already running `0.25.0-4049571`, which contains the Launcher; it
was simply inert because neither host's `KDESKDASH_MODES` named it.

## What shipped

- **[deploy/redis-kvscf.conf](../deploy/redis-kvscf.conf)** +
  **[.service](../deploy/redis-kvscf.service)** — rpidash3's kvscf-feed instance
  on 6380: ephemeral (32mb, allkeys-lru, no persistence), bound to loopback plus
  this board's LAN address only, `requirepass` on.
- **[deploy/hosts/rpidash3.env](../deploy/hosts/rpidash3.env)** — `launcher`
  added at the head of the ops list, and `KDESKDASH_KVSCF_REDIS_*` pointed at
  `127.0.0.1:6380` (the seam WI #664 built, finally used).
- **[deploy/hosts/rpidash2.env](../deploy/hosts/rpidash2.env)** — `launcher`
  added to the ops list, per Ken's deploy note on #1137.
- **[docs/kwork-rpidash3-pairing.md](../docs/kwork-rpidash3-pairing.md)** — the
  runbook: topology, why it is shaped this way, bring-up on all three machines,
  four verification steps, rollback.
- Secrets surface documented in
  [deploy/hosts/README.md](../deploy/hosts/README.md),
  [kdeskdash.env.example](../deploy/kdeskdash.env.example), the README env table
  and CLAUDE.md.

## Decisions

**The publisher was the side that could not authenticate, and nobody had
checked.** WI #1137 said the security work was low-risk because "both sides are
already proven — kdeskdash's client takes an `auth` argument and rpi53's
instance already requires one." Both halves of that are true and neither is the
relevant side: kwork's **kvscf** is what connects to this Redis, and it built a
bare `redis://{host}:{port}` with no password field at all. Found by reading
kvscf's `remote.rs` rather than trusting the WI. kvscf shipped
`KVSCF_REDIS_PASSWORD` the same day (its sprint 018, PR #20, `480df2b`), so the
blocker cleared inside the sprint — but the lesson is that "both sides are
proven" named the two sides that were not in the path.

**A second instance, not `requirepass` on 6379.** The WI had the kvscf feed
riding rpidash3's existing control Redis. That would have put mode control,
last-mode persistence, the GoL injection and the screenshot trigger on a
LAN-bound listener — precisely what rpidash2's 6379/6380 split exists to avoid,
and `deploy/redis-claude.conf` says so in its own comments. rpidash3 now runs
the same split. A side benefit: because the control instance stayed loopback-only
and passwordless, kdeskdash's control connection needed no `REDISCLI_AUTH`, and
the trap where you set the kvscf auth and forget the control one never opened.

**"Corporate LAN" described the machine, not the network.** rpidash3 is
`192.168.1.73` — the same home /24 that kvscf already hardcodes for rpidash2. The
`requirepass` stayed anyway, on three arguments that survive the correction:
rpidash3 is dual-homed and kwork cannot join the tailnet, so this listener sits
outside the tailnet ACLs covering every other homelab hop; the full kvscf build
publishes work Edge/VS Code window titles, which the fleet feed does not; and it
now costs nothing on either side.

**The password and the LAN bind live in an uncommitted include, and Redis fails
to start without it.** `include /etc/redis/redis-kvscf-local.conf` is the last
line of the committed conf. That keeps the secret and the host-specific address
out of the repo, and makes the failure mode fail *closed* — "came up without the
local file" can never quietly mean "came up LAN-bound and open". Verified by
moving the file and confirming the unit refuses to start, rather than assuming.

## Verified on hardware

The pairing's far end is a machine this repo's tooling cannot reach, so the read
path was proven with a synthetic publisher standing in for kwork — kdeskdash
cannot tell the difference, and it means the only unknown left for Ken is
kvscf's own config.

**Isolation, from another host on the LAN:**

| Check | Result |
|---|---|
| control `6379` from kai | `Connection refused` — still loopback-only |
| feed `6380` from kai | `NOAUTH Authentication required` — reachable, and closed |
| feed `6380` on-box with the password | `PONG` |
| unit with the local include removed | fails to start, `journalctl` names it |

**The panel, on rpidash3** (`kvscf:launcher:kwork`, published to the
authenticated endpoint at the contract's ~1s cadence / 10s TTL):

- 5 valid buttons rendered with correct placements and `w:2` spans; the 3
  invalid ones skipped and logged once — `launcher: skipped 3 invalid button(s)
  from kwork`. Two were deliberately bad (off-grid, overlapping); the third was
  a genuine mistake in the test payload (`h:2` starting at row 2 of a 3-row
  grid), which is a better result than the two planted ones — the validator
  caught something its author did not.
- All three colour paths, through the authenticated endpoint: `#2ec4c4` hex,
  `EDGE_TEAL` and `zombie_rust` palette names (case-insensitive both ways), and
  `chartreuse` / `""` falling to the default rather than failing the button.
- `🦀 Rust Docs` rendered as `Rust Docs` — the label filter working on a real
  panel, no tofu.
- **Cache-and-dim confirmed on this board**: with the key deleted, the buttons
  stayed, greyed, and the pane showed `kvscf offline` in amber. This is the
  behaviour the whole design rests on, because the machine publishing it sleeps
  and locks all day.

**On rpidash2**: 10 modes registered, Launcher connects and reads
`kvscf:launcher:cleo` — which cleo currently publishes as a *valid but empty*
grid (`"buttons":[]`). The empty grid on the dev desk is correct behaviour, not
a fault: no buttons are configured on cleo yet.

## Token rotation (#1144), folded in

Done at the same time because this sprint was minting kwork's token anyway, and
#1144 itself notes doing both at once is cheaper than a week apart.

- cleo's `KVSCF_TOKEN` rotated in `HKCU\Software\kenhia\kvscf`, and rpidash2's
  `/etc/kdeskdash/secrets.env` updated to match.
- **The stale fallback was real.** `C:\tools\bin\.env` existed and held exactly
  one key: a duplicate `KVSCF_TOKEN`. Removed — after rotation the value in it
  was dead, so keeping a backup would only have left a dead secret on disk.
  The registry is now the single source, which is what #1144 asked for.
- kwork's own token and the Redis password were generated **on rpidash3** and
  never printed, since a token reaching a session transcript is the entire
  reason #1144 exists. Both are in `/etc/kdeskdash/secrets.env` (0600) and
  repeated for kwork's setup in `/etc/kdeskdash/kvscf-pairing.txt` (root, 0600),
  which Ken deletes once kwork is configured.

Rotation is **not complete until kvscf on cleo is relaunched** — it runs from
`C:\tools\bin\kvscf.exe` with no Run key or scheduled task, so restarting it
from ssh risked killing a GUI app on a live desktop with no way to bring it
back. Until then cleo's kvscf holds the old token in memory and rpidash2's taps
do nothing; the feeds keep flowing throughout, which is exactly the silent
failure #1144 warns about.

## Follow-ups

- **kwork itself** — the one part no machine here can reach. Install the full
  `kvscf` build (kwork has been on `kvscf-local`, which has the comms module
  compiled out and cannot publish), set the five registry values, unplug the
  Stream Deck. Runbook §3.
- **Relaunch kvscf on cleo** to finish the rotation, then verify a tap actually
  foregrounds a window — the feeds cannot tell you.
- **Configure cleo's launcher buttons**, or accept an empty grid on the dev desk.
- **krot**: register the rpidash3 Redis password and both tokens — locations and
  fingerprints, never values. #1144 is the natural home.
- **`kvscf-local` has no consumer left** once kwork moves to the full build. Not
  this sprint's call; note that it still earns its keep as a lint target
  (`cargo clippy -p kvscf-local` sees publisher-only dead fields a workspace
  build structurally cannot).
