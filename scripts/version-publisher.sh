#!/usr/bin/env bash
# The one place the PUBLISHER BUNDLE's version string is derived.
#
#   <publisher/VERSION>-<short commit touching the bundle payload>
#
# Not the dashboard's version: the publisher changes on its own clock, and a
# sprint that touches one rarely touches the other. Scoping the sha to the
# payload means the version moves exactly when the shipped content does, so
# `just publish-publisher` can ask the store whether it already has this
# version and answer "nothing to publish" (korg WI 2801) instead of letting
# kpkg's immutability guard refuse it — a correct answer in the wrong shape,
# which sprint-ship's Phase 7 reads as a deploy failure.
#
# This exists so the payload list has ONE home. It was briefly inline in both
# scripts/publish-publisher.sh and the justfile, which is two places for one
# fact and exactly how a publish and its read-back come to disagree.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)

# Keep in step with the `payload` array in scripts/publish-publisher.sh — that
# is what is STAGED, this is what is VERSIONED, and they must name the same
# files. publisher/VERSION rides here too: a base bump must move the sha.
payload=(
    publisher/claude-pub.sh
    publisher/ghcp-pub.sh
    publisher/ghcp-hooks.json
    publisher/ghcp-hooks.darwin.json
    publisher/kdeskdash-claude-poll.service
    publisher/kdeskdash-claude-poll.timer
    publisher/VERSION
)

base=$(head -n1 "$repo/publisher/VERSION" | tr -d '[:space:]')
sha=$(git -C "$repo" log -1 --format=%h -- "${payload[@]}" 2>/dev/null || true)
if [ -z "$sha" ]; then
    echo "version-publisher: no commit touches the payload — is this a git checkout?" >&2
    exit 1
fi
printf '%s-%s\n' "$base" "$sha"
