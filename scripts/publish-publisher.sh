#!/usr/bin/env bash
# Publish the claude-feed publisher to the homelab package store.
#
#   scripts/publish-publisher.sh
#
# Invoked by `just publish-publisher`. Stages exactly what k-homelab installs
# on feed hosts and hands it to `kpkg` on the store host as
# artifacts/kdeskdash-publisher/<version>/.
#
# Doctrine: k-homelab docs/deploying.md — dev iteration in the repo is fine,
# cross-machine consumption goes through the store. The publisher used to cross
# that boundary two other ways (a vendored copy in k-homelab that went stale,
# and staging from a checkout on kai); this recipe is what retires both.
#
# The version is <publisher/VERSION>-<short sha of the last commit touching
# the payload>, NOT the dashboard's version: the publisher changes on its own
# clock. Scoping the sha to the payload means the version moves exactly when
# the shipped content does — republishing an unchanged publisher reproduces
# the version already in the store, and kpkg's immutability guard refusing it
# is the correct answer ("already published"), not an obstacle.
#
# Env (from .env on the dev box, no default on purpose — a guessed hostname
# fails later as a confusing ssh error instead of here as a sentence):
#   KDESKDASH_STORE_HOST   host running kpkg (kubsdb)
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo"

: "${KDESKDASH_STORE_HOST:?set KDESKDASH_STORE_HOST in .env (the host running kpkg, e.g. kubsdb)}"

# What ships: the script and the two poll units — both halves of what
# k-homelab consumes, so no recipe is left staging anything from a checkout.
# publisher/VERSION rides in the tracked set (a base bump must move the sha)
# but is not shipped as-is; the bundle gets the full version string instead.
payload=(
    publisher/claude-pub.sh
    publisher/kdeskdash-claude-poll.service
    publisher/kdeskdash-claude-poll.timer
)

# A published version names a commit or it names nothing (same rule as
# scripts/publish.sh, and the same reason).
if [ -n "$(git status --porcelain)" ]; then
    echo "publish-publisher: refusing to publish from a dirty tree — a published version must name a commit" >&2
    exit 1
fi

base=$(head -n1 publisher/VERSION | tr -d '[:space:]')
sha=$(git log -1 --format=%h -- "${payload[@]}" publisher/VERSION)
if [ -z "$sha" ]; then
    echo "publish-publisher: no commit touches the payload — is this a git checkout?" >&2
    exit 1
fi
v="$base-$sha"

# A branch commit vanishes from history at squash-merge, so a branch build may
# exist in the store (to prove the path works) but must never become `latest`.
latest_arg=""
if [ "$(git rev-parse --abbrev-ref HEAD)" != "main" ]; then
    latest_arg="--no-latest"
    echo "publish-publisher: not on main — publishing $v WITHOUT moving the latest pointer" >&2
fi

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
cp "${payload[@]}" "$stage/"
# The full version string rides in-band so an install can answer "what is on
# this host" without the store path that delivered it.
printf '%s\n' "$v" > "$stage/VERSION"

echo "==> publishing kdeskdash-publisher $v to $KDESKDASH_STORE_HOST"
d=$(ssh -n "$KDESKDASH_STORE_HOST" mktemp -d)
scp -q "$stage"/* "$KDESKDASH_STORE_HOST:$d/"
# shellcheck disable=SC2029  # $d and $v are ours, expanded here on purpose
ssh -n "$KDESKDASH_STORE_HOST" "kpkg artifact $latest_arg kdeskdash-publisher $v $d/* && rm -rf $d"

echo "published: kdeskdash-publisher $v"
