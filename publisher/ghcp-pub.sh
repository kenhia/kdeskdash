#!/bin/bash
# ghcp-pub.sh — kdeskdash GitHub Copilot CLI session publisher.
#
# The sibling of claude-pub.sh, not a fork of it: same transport (`kdash-pub`
# at its fleet-installed absolute path, batch mode, --stem KDASH_CLAUDE_REDIS),
# same key sanitising, same 2 h TTL and keepalive throttle, same
# fire-and-forget posture. What differs is the input — Copilot's hook payloads
# are not Claude's — and what the feed can honestly say.
#
# Contract: kdashdata contracts/schemas/ghcp-session.schema.json (CD-21).
#   ghcp:session:<host>:<sid>   HASH, TTL 7200 s; sessionEnd DELs the key.
# `ghcp:recent` is deliberately NOT written: the registry keeps it optional and
# unschema'd, and a publisher inventing an unschema'd family is a contract
# decision this script does not get to make.
#
# Usage:  ghcp-pub.sh <event>        stdin = the hook's payload JSON
#
# THE EVENT NAME IS AN ARGUMENT, and that is the single biggest shape
# difference from claude-pub.sh. Claude Code puts `hook_event_name` in the
# payload; Copilot does not put the event name anywhere at all, so the only
# thing that knows which event fired is the hook declaration that invoked us.
# ghcp-hooks.json passes it. There is one mode, so the mode word claude-pub.sh
# carries ("hook") would be a constant and is left out.
#
# WHAT THIS FEED CANNOT SAY, measured on Copilot CLI 1.0.85 on kai (sprint 038,
# re-confirming sprint 012's probe on 1.0.83 — same six events, same order):
#   * There is NO turn-end hook. The complete user-level set is sessionStart,
#     sessionEnd, userPromptSubmitted, preToolUse, postToolUse, errorOccurred,
#     and nothing fires when the agent finishes replying. So this publisher
#     raises `working` and clears the key, and can NEVER emit `awaiting`.
#     A Copilot session really waiting on its user keeps saying `working`
#     until the reader's freshness ladder ages it. That is the feed's known
#     blind spot and the reason the ladder is not optional for this family.
#   * `blocked` has no source either, and sprint 038 measured the approval
#     path rather than assuming it. A tool that is refused fires preToolUse
#     and then NO postToolUse at all (3 pre / 0 post, measured) — so
#     preToolUse fires BEFORE the permission decision, not after it, and is
#     therefore the last event before a session sits on an approval dialog.
#     It is also the event before every auto-approved tool, and it carries
#     nothing that separates the two. claude-pub.sh can publish `blocked`
#     because AskUserQuestion is one specific tool whose whole purpose is to
#     block; Copilot's block is a property of the permission system, not of a
#     tool. Emitting `blocked` on preToolUse and `working` on postToolUse
#     would therefore mark every long-running build as blocked, which is the
#     busiest a session ever is — so it is not done. The schema's "never
#     emits blocked" stands.
#
# Fire-and-forget: network I/O is backgrounded, failures are silent, exit is
# always 0 — a dead Redis must never slow a Copilot session down.

LC_ALL=C
export LC_ALL

# The publisher CLI, at the fleet-installed absolute path (kdashdata CD-13).
# Absolute, not a PATH lookup: a hook context's PATH is not the interactive
# shell's. Not a per-user copy either — that would bypass the store, so a
# `knarr deploy kdash-pub` upgrade would never reach the hooks.
KDD_PUB_BIN="${KDD_PUB_BIN:-}"
if [ -z "$KDD_PUB_BIN" ]; then
  for _kdd_c in /usr/local/bin/kdash-pub /c/tools/bin/kdash-pub.exe ; do
    [ -x "$_kdd_c" ] && { KDD_PUB_BIN="$_kdd_c" ; break ; }
  done
fi
# Checked however it was chosen, override included: an override naming a file
# that is not there would otherwise fail at exec time with output already
# redirected to /dev/null — publishing nothing, and saying nothing about it.
[ -x "$KDD_PUB_BIN" ] || KDD_PUB_BIN=""

# One home. `ghcp:*` rides the claude family's stem by the registry's own row
# (KDASH_CLAUDE_REDIS carries `claude:*` and `ghcp:*`), because CD-21 put this
# family on the central Redis beside the one it mirrors.
KDD_LEGS="${KDD_LEGS:-claude}"

KDD_TTL_S=7200
KDD_HEARTBEAT_MIN_S=120 # tool-use keepalive throttle (panel greys at 15m)

# The schema's own bound on ts/started_ts: 1e11 seconds is the year 5138, and
# every millisecond stamp after 1973 exceeds it. Named here because this
# script's job is to stay inside it, not merely to know about it.
KDD_TS_MAX=100000000000

# Overridable so the batch-shape test can run without touching a real
# install's throttle state (and vice versa).
STATE_DIR="${KDD_STATE_DIR:-${HOME}/.copilot/kdeskdash-pub/state}"

# ---------- tiny JSON helpers (flat fields on a single-line document) ----------
#
# FIRST match, not last, and that is the point of `grep -o` here rather than
# the greedy `sed` claude-pub.sh uses. Copilot's tool events carry `toolArgs`,
# whose contents are a tool's arguments and therefore arbitrary — a bash
# command containing `"timestamp":0` is a thing a user can cause. Every field
# this script reads is top-level and Copilot emits top-level fields first, so
# taking the leftmost match reads the payload and never its cargo.

# jstr <json> <field>: first "field":"value", minimally unescaped.
jstr() {
  printf '%s' "$1" \
    | grep -oE "\"$2\"[[:space:]]*:[[:space:]]*\"([^\"\\\\]|\\\\.)*\"" \
    | head -n1 \
    | sed -e 's/^[^:]*:[[:space:]]*"//' -e 's/"$//' \
          -e 's/\\"/"/g' -e 's/\\\\/\\/g' -e 's/\\\//\//g'
}

# jnum <json> <field>: first numeric field.
jnum() {
  printf '%s' "$1" \
    | grep -oE "\"$2\"[[:space:]]*:[[:space:]]*-?[0-9][0-9.]*" \
    | head -n1 | sed 's/.*:[[:space:]]*//'
}

# sanitized token for key material (host/session id)
token() { printf '%s' "$1" | tr -cd 'A-Za-z0-9._-' | cut -c1-63; }

# ---------- transport: kdash-pub batch ----------
#
# Commands accumulate as tab-separated lines — kdash-pub's `batch` format, which
# needs no quoting rules because a JSON string cannot contain a literal tab —
# and go out in one connection. `--best-effort` maps a delivery failure to exit
# 0, because a dead Redis must never fail a hook; an off-contract key still
# exits 1, which is a bug worth noticing.

BATCH=""

# cmd <verb> <arg…>: append one command. Tab/CR/LF are stripped from every field
# because they are the format's only delimiters, and nothing published here
# legitimately contains one.
cmd() {
  local a first=1
  for a in "$@"; do
    a="${a//[$'\t\r\n']/}"   # pure bash: this runs on every tool call, forks do not
    if [ "$first" = 1 ]; then BATCH+="$a" ; first=0 ; else BATCH+=$'\t'"$a" ; fi
  done
  BATCH+=$'\n'
}

# The stem naming one home, into $LEG_STEM; non-zero for a name we do not know,
# so an unknown leg publishes nowhere rather than guessing at a home.
LEG_STEM=""
leg_stem() {
  case "$1" in
    claude)  LEG_STEM=KDASH_CLAUDE_REDIS ;;
    central) LEG_STEM=KDASH_CENTRAL_REDIS ;;
    *)       return 1 ;;
  esac
}

# Write $BATCH to one home.
send_leg() {
  leg_stem "$1" || return 0
  printf '%s' "$BATCH" | "$KDD_PUB_BIN" --app kdeskdash \
    --stem "$LEG_STEM" --best-effort batch
}

# A host with no kdash-pub publishes nothing — the failure the store install
# exists to prevent. Leave a breadcrumb a human can find instead of a hook that
# prints, and never let it cost more than the file write.
no_binary() {
  printf 'no kdash-pub at /usr/local/bin or /c/tools/bin as of %s\n' "$NOW" \
    > "${STATE_DIR}/no-kdash-pub" 2>/dev/null
  BATCH=""
}

# Send to every leg, in parallel, and wait. Used where the process is about to
# die (sessionEnd): a backgrounded sender loses the race with CLI exit / ssh
# teardown and the DEL never arrives (ghost session until TTL).
send_sync() {
  [ -n "$BATCH" ] || return 0
  [ -n "$KDD_PUB_BIN" ] || { no_binary ; return 0 ; }
  local leg
  for leg in ${KDD_LEGS//,/ }; do
    send_leg "$leg" &
  done
  wait
  BATCH=""
}

# Disowned fire-and-forget variant so live-session events never wait. The child
# forks with $BATCH already set, so clearing it here cannot race the send.
send() {
  [ -n "$BATCH" ] || return 0
  [ -n "$KDD_PUB_BIN" ] || { no_binary ; return 0 ; }
  send_sync >/dev/null 2>&1 &
  disown 2>/dev/null
  BATCH=""
}

# ---------- shared context ----------

HOST=$(token "${HOSTNAME:-${COMPUTERNAME:-$(hostname 2>/dev/null)}}")
HOST="${HOST%%.*}"
[ -n "$HOST" ] || HOST=unknown
NOW=$(date +%s)

# project name from a cwd that may be POSIX or Windows style
project_of() {
  local p="$1"
  p="${p%[/\\]}"
  p="${p##*[/\\]}"
  printf '%s' "${p:-?}"
}

# Unix SECONDS from the payload's own stamp. THE UNIT IS THE TRAP ON THIS FEED:
# Copilot delivers `timestamp` in milliseconds (measured: 1789622590920), and a
# pass-through publishes a stamp ~1000x further into the future than now. Every
# reader treats a negative age as clock skew and therefore fresh, so such a
# record reads as permanently, convincingly live with nothing reporting an
# error. The schema bounds the field for exactly that reason; this converts by
# the same bound rather than by counting digits, so it stays correct if Copilot
# ever changes the unit.
ts_of() {
  local v="${1%%.*}"
  case "$v" in ''|*[!0-9]*) printf '%s' "$NOW" ; return ;; esac
  [ "$v" -gt "$KDD_TS_MAX" ] && v=$(( v / 1000 ))
  # Still out of range (or zero) means the stamp is not usable at all; our own
  # clock is a worse answer than the payload's but a far better one than a
  # record the schema rejects.
  if [ "$v" -le 0 ] || [ "$v" -gt "$KDD_TS_MAX" ]; then printf '%s' "$NOW" ; return ; fi
  printf '%s' "$v"
}

mkdir -p "$STATE_DIR" 2>/dev/null

# ---------- keepalive ----------
#
# A ts-only refresh, throttled per session. The EXPIRE rides along because the
# TTL is on the HASH, not on the write, so a bare ts update against a live key
# must re-arm it. At a 2-minute cadence against the panel's 15-minute greying
# threshold, seven writes cover a window and a single dropped one can never
# grey a row.
heartbeat() {
  local sid="$1" key="$2" ts="$3" hb
  hb=$(cat "${STATE_DIR}/${sid}.hb" 2>/dev/null | tr -cd '0-9')
  [ -n "$hb" ] && [ $((NOW - hb)) -lt "$KDD_HEARTBEAT_MIN_S" ] && return 0
  printf '%s' "$NOW" > "${STATE_DIR}/${sid}.hb" 2>/dev/null
  cmd hset "$key" ts "$ts"
  cmd expire "$key" "$KDD_TTL_S"   # never leave a TTL-less key behind
  send
}

# ---------- main ----------

event="${1:-}"
[ -n "$event" ] || exit 0

json=$(cat)
sid=$(token "$(jstr "$json" sessionId)")
[ -n "$sid" ] || exit 0
key="ghcp:session:${HOST}:${sid}"
ts=$(ts_of "$(jnum "$json" timestamp)")

# Keepalive fast path, taken before any further parsing. preToolUse and
# postToolUse fire on every tool call in every session on the box, which makes
# this the hottest path in the script, so it pays for only what it needs.
# Unlike claude-pub.sh there is no tool to branch on: Copilot has no
# AskUserQuestion, and the header says why no tool event can mean `blocked`.
case "$event" in
  preToolUse|postToolUse)
    heartbeat "$sid" "$key" "$ts"
    exit 0
    ;;
esac

cwd=$(jstr "$json" cwd)
project=$(project_of "$cwd")

case "$event" in
  sessionStart)
    # started_ts comes from THE PAYLOAD and never from our own clock, because
    # the two hooks do not fire in the order their names suggest: measured on
    # both 1.0.83 and 1.0.85, userPromptSubmitted fires ~5 ms BEFORE
    # sessionStart in -p mode. Treating sessionStart as "the first write"
    # would clobber a record that already exists; reading the stamps is
    # correct either way round, and hset merges fields rather than replacing
    # the hash.
    cmd hset "$key" host "$HOST" project "$project" cwd "$cwd" \
               status working ts "$ts" started_ts "$ts"
    cmd expire "$key" "$KDD_TTL_S"
    ;;
  userPromptSubmitted)
    cmd hset "$key" host "$HOST" project "$project" cwd "$cwd" \
               status working ts "$ts"
    cmd expire "$key" "$KDD_TTL_S"
    ;;
  sessionEnd)
    # No `ghcp:recent` counterpart to claude:recent — see the header. The DEL
    # is the whole of this event, so it needs no batch of its own, and it goes
    # synchronously because the CLI is exiting and a backgrounded DEL loses
    # that race (ghost session until the 2 h TTL).
    cmd del "$key"
    rm -f "${STATE_DIR}/${sid}.hb" 2>/dev/null
    find "$STATE_DIR" -type f -mtime +2 -delete 2>/dev/null
    send_sync
    exit 0
    ;;
  *)
    # errorOccurred and anything Copilot adds later: no status this feed can
    # honestly publish, so nothing is written. Silence beats a guess.
    exit 0
    ;;
esac

send
exit 0
