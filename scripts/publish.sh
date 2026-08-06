#!/usr/bin/env bash
# Publish a kdeskdash release to the homelab package store.
#
#   scripts/publish.sh
#
# Invoked by `just publish`. Cross-compiles the aarch64 binary, stages it with
# everything a board needs to run that exact build, and hands the set to `kpkg`
# on the store host as artifacts/kdeskdash/<version>/.
#
# Doctrine: k-homelab docs/deploying.md — *every deploy publishes a versioned
# asset to the store; every install and deploy pulls from the store*. What is
# specific to kdeskdash is in docs/deploying.md here.
#
# Why the font rides along (2.4MB, byte-identical release after release): a
# version directory has to be able to put a board back the way it was, and the
# icons mode is unavailable without it. Splitting it into a separately-versioned
# asset would save NAS bytes nobody is short of and cost the property that makes
# rollback one command.
#
# Env (from .env on the dev box, no defaults on purpose — a guessed hostname
# fails later as a confusing ssh error instead of here as a sentence):
#   KDESKDASH_STORE_HOST   host running kpkg (kubsdb)
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo"

: "${KDESKDASH_STORE_HOST:?set KDESKDASH_STORE_HOST in .env (the host running kpkg, e.g. kubsdb)}"

# A published version names a commit or it names nothing. Everything in the
# store must be recoverable, and "the tree as it happened to be on someone's
# dev box" is not.
if [ -n "$(git status --porcelain)" ]; then
    echo "publish: refusing to publish from a dirty tree — a published version must name a commit" >&2
    exit 1
fi

v=$(scripts/version.sh)
case "$v" in
    *dirty* | *unknown*)
        echo "publish: version '$v' names no reproducible commit" >&2
        exit 1
        ;;
esac

# A branch commit vanishes from history at squash-merge, so a branch build may
# exist in the store (to prove the path works) but must never become what the
# fleet gets when it asks for `latest`.
latest_arg=""
if [ "$(git rev-parse --abbrev-ref HEAD)" != "main" ]; then
    latest_arg="--no-latest"
    echo "publish: not on main — publishing $v WITHOUT moving the latest pointer" >&2
fi

echo "==> building kdeskdash $v for aarch64"
cmake -B build-pi -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake -DKD_VERSION="$v"
cmake --build build-pi --target kdeskdash -j"$(nproc)"

# Name the binary with the target triple: the store holds one artifact tree per
# name, and a second architecture (or a Go binary next door) must be able to sit
# beside this one without either having to be renamed first.
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
cp build-pi/kdeskdash "$stage/kdeskdash-aarch64-linux"
cp fonts/ttf/SymbolsNerdFont-Regular.ttf "$stage/"
cp deploy/kdeskdash.service deploy/kdeskdash.env.example "$stage/"
# The installer ships inside the artifact so the push logic that shipped with a
# build stays recoverable with it. Per-host env files deliberately do NOT: they
# are device config that evolves on its own clock, install-service never
# overwrites one, and the repo is their source of truth.
cp scripts/deploy.sh "$stage/"

echo "==> publishing kdeskdash $v to $KDESKDASH_STORE_HOST ($(du -sh "$stage" | cut -f1))"
d=$(ssh -n "$KDESKDASH_STORE_HOST" mktemp -d)
scp -q "$stage"/* "$KDESKDASH_STORE_HOST:$d/"
# shellcheck disable=SC2029  # $d and $v are ours, expanded here on purpose
ssh -n "$KDESKDASH_STORE_HOST" "kpkg artifact $latest_arg kdeskdash $v $d/* && rm -rf $d"

echo "published: kdeskdash $v"
echo "  deploy it: just deploy rpidash2 $v"
