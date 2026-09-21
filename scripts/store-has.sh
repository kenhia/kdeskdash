#!/usr/bin/env bash
# Is a version already in the homelab package store?
#
#   scripts/store-has.sh <artifact> <version>
#
# Exposed as `just published <version>` / `just published-publisher <version>`,
# and used by both publish recipes to skip a publish that would republish
# byte-identical content (korg WI 2801).
#
# THREE outcomes, because two of them are not the same answer:
#
#   0  present        the store has it — publishing again is a no-op
#   1  absent         the store does not have it — publish
#   2  could-not-ask  the store could not be reached, or did not answer
#
# `ssh … test -d` is the tempting one-liner and it is the trap: an absent
# version, a downed store host, a missing host key and a refused login all exit
# non-zero, and reading any of them as "absent" would republish over a store
# nobody could see — a false claim about the world rather than a failed command.
# So the remote answers with a WORD it prints itself, and nothing is concluded
# from an exit status alone. A caller that cannot tell 1 from 2 must treat 2 as
# a refusal, which is what both publish recipes do.
#
# Env (from .env on the dev box):
#   KDESKDASH_STORE_HOST   host running kpkg (kubsdb)
set -uo pipefail

# --say makes the verdict a sentence as well as an exit code, for `just
# published`. Off by default: the publish recipes print their own, tailored to
# what they are about to do or not do, and two messages for one fact reads like
# two facts.
say=0
if [ "${1:-}" = "--say" ]; then say=1; shift; fi

artifact="${1:-}"
version="${2:-}"
if [ -z "$artifact" ] || [ -z "$version" ]; then
    echo "usage: store-has.sh [--say] <artifact> <version>" >&2
    exit 2
fi

verdict() {  # <exit-code> <message>
    [ "$say" = 1 ] && { if [ "$1" = 2 ]; then echo "$2" >&2; else echo "$2"; fi; }
    exit "$1"
}

# Not `${VAR:?}`: that exits 1, which this script has just spent a paragraph
# saying means "absent". An unconfigured store host is the plainest
# could-not-ask there is, and answering it with "absent" would republish.
if [ -z "${KDESKDASH_STORE_HOST:-}" ]; then
    echo "store-has: KDESKDASH_STORE_HOST is unset — set it in .env (the host running kpkg, e.g. kubsdb)" >&2
    exit 2
fi

# BatchMode so a host that would prompt fails fast instead of hanging a deploy,
# and a bounded connect so an unreachable store is answered in seconds.
out=$(ssh -n -o BatchMode=yes -o ConnectTimeout=10 "$KDESKDASH_STORE_HOST" \
        'kpkg list && echo KDD_LIST_OK' 2>/dev/null)

# The sentinel is the whole point: it is printed only if `kpkg list` itself
# succeeded on the far side, so an ssh failure, a missing kpkg and an empty
# listing are all distinguishable from a real answer.
case "$out" in
    *KDD_LIST_OK*) ;;
    *)
        verdict 2 "could not ask $KDESKDASH_STORE_HOST whether it has $artifact $version"
        ;;
esac

# `artifacts/<name>: v1, v2, v3 (latest: v2)`. An artifact with no line at all
# has never been published, which IS an answer — the listing succeeded.
line=$(printf '%s\n' "$out" | sed -n "s|^artifacts/${artifact}: ||p")
[ -n "$line" ] || verdict 1 "absent: $artifact has never been published"

line="${line%% (latest:*}"
case ",${line// /}," in
    *",${version},"*) verdict 0 "present: $artifact $version is already in the store" ;;
    *)                verdict 1 "absent: $artifact $version is not in the store" ;;
esac
