#!/usr/bin/env bash
# The one place a kdeskdash version string is derived.
#
#   <VERSION file>-<short commit>[-dirty]     e.g. 0.27.0-1a2b3c4
#
# The minor tracks the sprint number (VERSION is bumped by the sprint that
# changes what ships). The commit is the last one touching the PAYLOAD — the
# files that end up in the published artifact, or that change the bytes of the
# binary inside it — and not HEAD.
#
# Scoping it to the payload is what makes `just publish` self-skipping (korg WI
# 2801): the version moves exactly when the shipped content does, so a sprint
# that changed only docs reproduces the version already in the store, and the
# recipe can answer "nothing to publish" instead of cutting a release and
# restarting both panels for a diff with no dashboard code in it. It is the
# same derivation `scripts/publish-publisher.sh` has always used for the
# publisher bundle, and the same rule klaude-top adopted (its WI 2782).
#
# `-dirty` is load-bearing, not decoration: `just push-dev` can put an
# uncommitted build on a panel, and that build must be visibly *not* a
# published artifact when `just versions` asks a board what it is running. It
# is scoped to the payload for the same reason the commit is — an edit to a doc
# or a per-host env file does not change the binary. `scripts/publish.sh`
# refuses a dirty tree of ANY kind independently, so the store is not relying
# on this.
#
# CMake does not derive this itself — it takes -DKD_VERSION from the recipe.
# A cached CMake variable would freeze at the value it was configured with,
# and a stamp that lies about the commit is worse than no stamp.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
base=$(head -n1 "$repo/VERSION" | tr -d '[:space:]')

# The payload. Deliberately NOT here: this script, scripts/publish.sh and the
# justfile. The rule is Ken's (2026-09-17), shared with klaude-top — the list
# names the files that SHIP, not the machinery that ships them, so a tooling
# edit does not cut a release. The cost is narrow and known: changing only the
# build flags in publish.sh's own cmake invocation would alter the binary
# without moving the version. The remedy is a VERSION bump; docs/deploying.md
# says so. (The toolchain file and its compiler options live in cmake/, which
# IS an input, so the gap is just that one invocation line.)
#
# Keep this in step with what scripts/publish.sh stages.
inputs=(
    src
    lib
    lv_conf.h
    CMakeLists.txt
    cmake
    fonts/ttf/SymbolsNerdFont-Regular.ttf
    deploy/kdeskdash.service
    deploy/kdeskdash.env.example
    scripts/deploy.sh
    VERSION
)

if ! git -C "$repo" rev-parse --git-dir >/dev/null 2>&1; then
    # No git (a tarball export): the base version is all that is knowable.
    printf '%s-unknown\n' "$base"
    exit 0
fi

sha=$(git -C "$repo" log -1 --format=%h -- "${inputs[@]}" 2>/dev/null || true)
if [ -z "$sha" ]; then
    # A checkout where no commit touches the payload at all. Not a version this
    # repo can publish, and publish.sh refuses `unknown` by name.
    printf '%s-unknown\n' "$base"
    exit 0
fi

dirty=""
[ -n "$(git -C "$repo" status --porcelain -- "${inputs[@]}")" ] && dirty="-dirty"

printf '%s-%s%s\n' "$base" "$sha" "$dirty"
