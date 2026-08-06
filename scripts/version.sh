#!/usr/bin/env bash
# The one place a kdeskdash version string is derived.
#
#   <VERSION file>-<short commit>[-dirty]     e.g. 0.24.0-1a2b3c4
#
# The minor tracks the sprint number (VERSION is bumped by the sprint that
# changes what ships), and the commit makes every publishable build a version
# the store has not seen — so `just publish` never needs a version bump to
# republish, which is the rule k-homelab docs/deploying.md asks each repo to
# wire into its own release path.
#
# `-dirty` is load-bearing, not decoration: `just push-dev` can put an
# uncommitted build on a panel, and that build must be visibly *not* a
# published artifact when `just versions` asks a board what it is running.
#
# CMake does not derive this itself — it takes -DKD_VERSION from the recipe.
# A cached CMake variable would freeze at the value it was configured with,
# and a stamp that lies about the commit is worse than no stamp.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
base=$(head -n1 "$repo/VERSION" | tr -d '[:space:]')

if ! sha=$(git -C "$repo" rev-parse --short HEAD 2>/dev/null); then
    # No git (a tarball export): the base version is all that is knowable.
    printf '%s-unknown\n' "$base"
    exit 0
fi

dirty=""
[ -n "$(git -C "$repo" status --porcelain)" ] && dirty="-dirty"

printf '%s-%s%s\n' "$base" "$sha" "$dirty"
