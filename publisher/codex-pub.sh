#!/bin/bash
# codex-pub.sh — kdeskdash Codex usage-limits publisher.
#
# The sibling of claude-pub.sh's `poll` mode, not a fork of it: same transport
# (`kdash-pub` at its fleet-installed absolute path, batch mode), same
# observation-time guard, same fire-and-forget posture. What differs is the
# source and the contract.
#
# Contract: kdashdata contracts/schemas/codex-limits.schema.json (CD-33).
#   codex:limits   HASH, no TTL, on KDASH_CENTRAL_REDIS (the registry's row).
#
# Usage:  codex-pub.sh poll        no stdin; one observation, one HSET
#
# Limits are account-wide, so ONE host runs this (kai, sprint 049). More
# writers would add freshness, not coverage — and the guard below means a
# second one can never publish over a newer reading anyway.
#
# SOURCES, measured on codex-cli 0.171.0 on kai (sprint 049):
#   * `codex app-server` JSON-RPC `account/rateLimits/read`. [experimental] in
#     Codex's own help, but the only source that works with no session running:
#     ~0.75 s, authenticated by Codex itself — this script never reads
#     auth.json. Logged out it answers a JSON-RPC error ("codex account
#     authentication required to read rate limits") in ~0.07 s, and that is
#     NOT a zero reading: nothing is published and the last one ages.
#     source=app-server.
#   * Fallback: the newest rollout's last `token_count` event, whose
#     `rate_limits` is the same snapshot in snake_case. Fresh only while a
#     session runs, so it is published with ITS OWN timestamp as updated_at —
#     an old rollout reads stale on the panel, never fresh. source=rollout.
#
# WINDOWS ARE KEYED BY LENGTH, never by slot (CD-33). On Ken's Pro (`prolite`)
# plan the PRIMARY slot holds the 10080-minute window and secondary is null:
# that is a seven-day reading with five_hour_none=1, not a five-hour one.
# A sample with no 10080-minute window is incomplete and publishes nothing.
#
# The app-server exits on stdin EOF before it answers, so a plain pipe loses
# the reply; the exchange runs as a coproc whose stdin stays open until the
# answer to request 2 arrives.
#
# Exit is always 0 — a timer unit that fails on a dead Redis is noise.

LC_ALL=C
export LC_ALL

# The publisher CLI, at the fleet-installed absolute path (kdashdata CD-13).
KDD_PUB_BIN="${KDD_PUB_BIN:-}"
if [ -z "$KDD_PUB_BIN" ]; then
  for _kdd_c in /usr/local/bin/kdash-pub /c/tools/bin/kdash-pub.exe ; do
    [ -x "$_kdd_c" ] && { KDD_PUB_BIN="$_kdd_c" ; break ; }
  done
fi
[ -x "$KDD_PUB_BIN" ] || KDD_PUB_BIN=""

# The registry puts codex:limits on central, beside claude:limits (CD-33).
KDD_STEM="${KDD_STEM:-KDASH_CENTRAL_REDIS}"

KDD_POLL_EXPECT_S=300   # the 5-minute timer; published as expected_refresh_s
KDD_RPC_TIMEOUT_S=20    # whole app-server exchange, start to answer

STATE_DIR="${KDD_STATE_DIR:-${HOME}/.codex/kdeskdash-pub/state}"
CODEX_DIR="${CODEX_HOME:-${HOME}/.codex}"

# ---------- tiny JSON helpers (flat fields on a single-line document) ----------

jstr() {
  printf '%s' "$1" \
    | grep -oE "\"$2\"[[:space:]]*:[[:space:]]*\"([^\"\\\\]|\\\\.)*\"" \
    | head -n1 \
    | sed -e 's/^[^:]*:[[:space:]]*"//' -e 's/"$//'
}

jnum() {
  printf '%s' "$1" | sed -n 's/.*"'"$2"'"[[:space:]]*:[[:space:]]*\(-\{0,1\}[0-9][0-9.]*\).*/\1/p' | head -n1
}

token() { printf '%s' "$1" | tr -cd 'A-Za-z0-9._-' | cut -c1-63; }

# iso_epoch <iso8601>: epoch seconds, or nothing (GNU date, then BSD date).
iso_epoch() {
  local s="$1" z
  [ -n "$s" ] || return 0
  date -u -d "$s" +%s 2>/dev/null | tr -cd '0-9' && [ "${PIPESTATUS[0]}" -eq 0 ] && return 0
  s=$(printf '%s' "$s" | sed -E 's/\.[0-9]+//')
  case "$s" in
    *Z) z=+0000; s=${s%Z} ;;
    *) z=+0000 ;;
  esac
  date -j -u -f '%Y-%m-%dT%H:%M:%S%z' "$s$z" +%s 2>/dev/null | tr -cd '0-9'
}

mtime() { { stat -c %Y "$1" 2>/dev/null || stat -f %m "$1" 2>/dev/null; } | tr -cd '0-9'; }

bounded() {
  if command -v timeout >/dev/null 2>&1; then timeout "$@"; else shift; "$@"; fi
}

HOST=$(token "${HOSTNAME:-${COMPUTERNAME:-$(hostname 2>/dev/null)}}")
HOST="${HOST%%.*}"
[ -n "$HOST" ] || HOST=unknown
NOW=$(date +%s)
mkdir -p "$STATE_DIR" 2>/dev/null

# Breadcrumbs, one file each, overwritten: a human can see why the gauge is
# stale without a journal, and a success clears them.
crumb() { printf '%s as of %s\n' "$2" "$NOW" > "${STATE_DIR}/$1" 2>/dev/null; }

# ---------- the sample ----------
#
# windows <snapshot-json> <pct-key> <mins-key> <reset-key>: fills P_* from the
# primary and secondary objects of one rate-limit snapshot, by duration. Both
# spellings come through here — app-server's camelCase and the rollout's
# snake_case — so the keys are arguments. Returns non-zero for an incomplete
# sample (no seven-day window), which publishes nothing.
P_FHN="" ; P_FH="" ; P_FHR="" ; P_SD="" ; P_SDR="" ; P_PLAN="" ; P_T="" ; P_SRC=""
windows() {
  local snap="$1" slot obj mins pct rst
  P_FHN=1 ; P_FH=0 ; P_FHR=0 ; P_SD="" ; P_SDR=0
  for slot in primary secondary; do
    obj=$(printf '%s' "$snap" | grep -oE "\"$slot\":\\{[^{}]*\\}" | head -n1)
    [ -n "$obj" ] || continue          # null slot, or absent
    mins=$(jnum "$obj" "$3") ; pct=$(jnum "$obj" "$2") ; rst=$(jnum "$obj" "$4")
    [ -n "$pct" ] || continue
    rst="${rst%%.*}"
    case "$mins" in
      300)   P_FHN=0 ; P_FH="$pct" ; P_FHR="${rst:-0}" ;;
      10080) P_SD="$pct" ; P_SDR="${rst:-0}" ;;
    esac                                # any other length: not in v1's projection
  done
  [ -n "$P_SD" ] || return 1
  P_PLAN=$(jstr "$snap" "$5")
  return 0
}

codex_bin() {
  local c
  for c in "${KDD_CODEX_BIN:-}" "$(command -v codex 2>/dev/null)" \
           "${HOME}/.local/bin/codex" /usr/local/bin/codex ; do
    [ -n "$c" ] && [ -x "$c" ] && { printf '%s' "$c"; return 0; }
  done
  return 1
}

# The rateLimits object, bounded: everything after `"rateLimits":{` up to the
# by-id map (the same snapshot again, which must not be read twice) or the end.
from_app_server() {
  local bin line resp snap
  bin=$(codex_bin) || { crumb codex-missing "no codex binary found"; return 1; }
  rm -f "${STATE_DIR}/codex-missing"
  coproc CX { cd "${TMPDIR:-/tmp}" && bounded "$KDD_RPC_TIMEOUT_S" "$bin" app-server 2>/dev/null; }
  local pid="$CX_PID" wfd="${CX[1]}"
  printf '%s\n' \
    '{"id":1,"method":"initialize","params":{"clientInfo":{"name":"kdeskdash-codex-pub","version":"1"},"capabilities":{"experimentalApi":true}}}' \
    '{"method":"initialized"}' \
    '{"id":2,"method":"account/rateLimits/read"}' >&"$wfd" 2>/dev/null
  while IFS= read -r -t "$KDD_RPC_TIMEOUT_S" line <&"${CX[0]}"; do
    # Key order is the server's, not ours: a result line leads with "id", an
    # error line ends with it (`{"error":{…},"id":2}`, measured logged out).
    case "$line" in
      '{"method"'*) ;;                                  # a notification
      *'"id":2,'*|*'"id":2}') resp="$line" ; break ;;
    esac
  done
  exec {wfd}>&- 2>/dev/null
  kill "$pid" 2>/dev/null ; wait "$pid" 2>/dev/null
  case "$resp" in
    *'"error"'*)
      crumb app-server-error "$(jstr "$resp" message)" ; return 1 ;;
    *'"rateLimits":{'*) ;;
    *) crumb app-server-error "no answer to account/rateLimits/read" ; return 1 ;;
  esac
  snap="${resp#*\"rateLimits\":\{}"
  snap="${snap%%\"rateLimitsByLimitId\"*}"
  windows "$snap" usedPercent windowDurationMins resetsAt planType || {
    crumb app-server-error "sample has no 10080-minute window" ; return 1 ; }
  rm -f "${STATE_DIR}/app-server-error"
  P_T="$NOW" ; P_SRC=app-server
  return 0
}

# Newest rollout by name (they are named by start time), then its last
# token_count event that carries rate_limits.
from_rollout() {
  local f rec snap t
  f=$(ls -1 "${CODEX_DIR}"/sessions/*/*/*/rollout-*.jsonl 2>/dev/null | sort | tail -n1)
  [ -n "$f" ] && [ -f "$f" ] || return 1
  rec=$(grep '"type":"token_count"' "$f" 2>/dev/null | grep '"rate_limits":{' | tail -n1)
  [ -n "$rec" ] || return 1
  t=$(iso_epoch "$(jstr "$rec" timestamp)")
  [ -n "$t" ] || return 1
  snap="${rec#*\"rate_limits\":\{}"
  windows "$snap" used_percent window_minutes resets_at plan_type || return 1
  P_T="$t" ; P_SRC=rollout
  return 0
}

# ---------- publish ----------

# The stored observation stamp, empty when absent or unreachable — on which the
# guard publishes (the same benign failure as claude-pub.sh's stored_epoch).
stored_epoch() {
  [ -n "$KDD_PUB_BIN" ] || return 0
  "$KDD_PUB_BIN" --app kdeskdash --stem "$KDD_STEM" --best-effort \
    hget codex:limits updated_at 2>/dev/null | tr -cd '0-9'
}

poll_mode() {
  local prev batch
  from_app_server || from_rollout || exit 0
  [ -n "$KDD_PUB_BIN" ] || { crumb no-kdash-pub "no kdash-pub at /usr/local/bin or /c/tools/bin"; exit 0; }

  # Never publish over a newer (or the same) observation: republishing a
  # sample must not advance updated_at (CD-33), and an old rollout must never
  # replace a fresher app-server reading.
  prev=$(stored_epoch)
  [ -z "$prev" ] || [ "$prev" -lt "$P_T" ] || exit 0

  # Every required field in one HSET (CD-33). With no 300-minute window the
  # five-hour pair is written as zeros alongside the flag, replacing any
  # earlier five-hour reading.
  batch="hset"$'\t'"codex:limits"
  batch+=$'\t'"five_hour_none"$'\t'"$P_FHN"
  batch+=$'\t'"five_hour_pct"$'\t'"$P_FH"
  batch+=$'\t'"five_hour_resets_at"$'\t'"$P_FHR"
  batch+=$'\t'"seven_day_pct"$'\t'"$P_SD"
  batch+=$'\t'"seven_day_resets_at"$'\t'"$P_SDR"
  batch+=$'\t'"updated_at"$'\t'"$P_T"
  batch+=$'\t'"expected_refresh_s"$'\t'"$KDD_POLL_EXPECT_S"
  batch+=$'\t'"host"$'\t'"$HOST"
  batch+=$'\t'"source"$'\t'"$P_SRC"
  [ -n "$P_PLAN" ] && batch+=$'\t'"plan_type"$'\t'"$(token "$P_PLAN")"
  printf '%s\n' "$batch" | "$KDD_PUB_BIN" --app kdeskdash \
    --stem "$KDD_STEM" --best-effort batch >/dev/null 2>&1
}

case "$1" in
  poll) poll_mode ;;
esac
exit 0
