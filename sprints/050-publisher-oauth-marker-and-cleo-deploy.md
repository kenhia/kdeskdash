# Sprint 050 — clear the stale oauth-expired marker, and a publisher path to cleo

**Proposal:** korg:5077 (slice of program korg:5082, "Low-hanging fruit,
2026-11-26").
**Covers:** WI 4793, WI 4651.
**Branch:** `050-publisher-oauth-marker-and-cleo-deploy`, off `fe24d92`.

Run as a karc leg on kai (`kdeskdash-740974`), overseen.

## Premise check

Checked 2026-11-26, about 18:15Z.

- **4793 holds.** In publisher 2.7.0 `state/oauth-expired` is removed only
  inside `oauth_refresh()`, and `from_oauth()` calls that only when
  `oauth_token` returns 2. A token made valid behind the poll's back (an
  interactive `claude auth login`) left the marker forever. A new test case
  reproduced it before the fix: `FAIL a fresh login clears a stale breadcrumb`.
- **4651 drifted, same direction.** cleo was no longer three releases behind:
  048 hand-copied 2.6.0-09bf6c9 there. It was still one release behind the
  store's `latest` (2.7.0-0e2457f), and nothing would have noticed. Its
  `claude-pub.sh` was already byte-identical to 2.7.0's, because 049 did not
  touch it; only `VERSION` differed.

## Decisions

- **Install the published artifact, not the checkout.** The recipe fetches the
  bundle from the store and checks it against `SHA256SUMS`, the same as the
  panel deploy. `fetch_artifact` in `scripts/deploy.sh` took an artifact name
  for this. The publisher caches under `.deploy-cache/kdeskdash-publisher/`,
  and `versions` filters that directory out of its panel list.
- **A `deploy.sh` subcommand (`publisher-windows`), not a new script.** It
  shares the fetch, the `die`, and the file's existing role as "the deploy
  script". `just deploy-publisher-cleo` is the named entry point Ken's ruling
  asked for.
- **Only `claude-pub.sh` and `VERSION`.** That is everything cleo runs from the
  bundle. `poll-hidden.vbs` is a one-time install and was never in the bundle.
  Adding it would change the store payload for the sake of a file that has not
  changed since August. A cleo with no install directory is refused, with a
  pointer to the README's manual steps, rather than half-installed.
- **PowerShell via `-EncodedCommand`.** cleo's ssh shell is PowerShell, and it
  parses the whole command string. Sending the scripts UTF-16LE/base64 means no
  quoting crosses the boundary at all.
- **Hash on cleo before and after the swap.** The staged `.new` files are
  checked against the artifact's sums before anything is renamed (a mismatch
  deletes them and changes nothing), and the installed files are hashed again
  afterwards. Rename retries for about five seconds, because a hook running the
  old script holds it open.
- **Self-skipping, but loud when cleo is away.** Matching hashes exit 0, so it
  is safe in `.sprint-deploy` unconditionally. An unreachable cleo fails the
  step and changes nothing. Silence is what let it fall three releases behind.
- **The kdashdata precedent has moved on.** kdashdata's cleo path is now
  `knarr deploy … --host-windows cleo`, not a hand recipe. Ken ruled option (a)
  with that on the table (option (b) was a fleet tool such as knarr), so this
  follows the ruling.

## What shipped

- `publisher/claude-pub.sh`: `from_oauth` removes `state/oauth-expired` as soon
  as it holds a valid token. Publisher **2.7.1**.
- `publisher/tests/batch-shape.sh`: marker present + valid token → one poll →
  marker gone, oauth published, no refresh attempted.
- `scripts/deploy.sh publisher-windows <target> [version]`, and `just
  deploy-publisher-cleo [version]`.
- `.sprint-deploy`: `recipe: deploy-publisher-cleo` after `publish-publisher`.
- `docs/deploying.md` (a "cleo" subsection, the 2.7.1 note),
  `publisher/README.md`, `CLAUDE.md` command list.

## Acceptance (triggered, same session, probed from kai)

- `just deploy-publisher-cleo` against the store's `latest` (2.7.0-0e2457f)
  moved cleo from `2.6.0-09bf6c9` to `2.7.0-0e2457f`. Only `VERSION` was
  swapped in substance; cleo's `VERSION` sha256 `0ec9a009…` now matches the
  store's sums, and no `.new` file was left behind.
- A second run: `already runs kdeskdash-publisher 2.7.0-0e2457f (hashes match)`,
  exit 0.
- An unreachable target: refused with the ssh error and "Nothing was changed",
  exit 1.
- 2.7.1 reaches cleo through the same recipe at ship, from `.sprint-deploy`.

## Not done here, on purpose

- **kai and kubs0** take the publisher through k-homelab's
  `kdeskdash_publisher_version` in `manifests/common.yml`. Another leg holds
  k-homelab. The pin bump is relayed through the overseer and not edited here.

## Repaired in passing

Nothing.

## Deployed

2026-11-26, about 18:25Z, from merged `main` (`d0e2b34`, PR #60), run from kai.
Ship turn of karc leg `kdeskdash-740974`, after the overseer cleared it
(korg:5077 comment 5171).

- **`deploy-panels`** published panel `0.28.0-d0e2b34`. This was a real
  release: `scripts/deploy.sh` is part of the panel payload. `just deploy`
  installed it on **rpidash2** and **rpidash3**. `just versions` reported both
  boards as `kdeskdash 0.28.0-d0e2b34`, both units were `active`, and `kddss`
  saved a frame from each board.
- **`just publish-publisher`** published `kdeskdash-publisher 2.7.1-d0e2b34`,
  and `latest` now points to it. Against 2.7.0-0e2457f the bundle diff, read
  off both SHA256SUMS files, is `claude-pub.sh` and `VERSION` only.
- **`just deploy-publisher-cleo`** fetched 2.7.1-d0e2b34 and installed it on
  cleo. The step reported "hashes verified on kenhi@cleo". A separate ssh read
  of cleo's files confirmed `VERSION` = `2.7.1-d0e2b34`. Their sha256 values
  are `d0a90893…` (claude-pub.sh) and `61a8b272…` (VERSION), which match the
  artifact's SHA256SUMS.
- **Live check of the marker fix (WI 4793), triggered on kai.** I ran the
  published 2.7.1 `claude-pub.sh poll` against kai's real CLI credentials, with
  a throwaway `KDD_STATE_DIR` that already held a stale `oauth-expired`. After
  the poll the marker was gone, and `claude:limits` read `source oauth` with
  `updated_at` equal to that second.
- **Not done here:** kai and kubs0 install the publisher through k-homelab's
  `kdeskdash_publisher_version`, which is still `2.7.0-0e2457f`. The bump to
  `2.7.1-d0e2b34` is relayed to the k-homelab leg through the overseer.
