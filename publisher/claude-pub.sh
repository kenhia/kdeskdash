#!/bin/bash
# claude-pub.sh — kdeskdash claude-feed publisher (zero-dependency).
#
# Publishes Claude Code session activity and subscription usage limits to the
# claude-feed Redis (rpidash2:6380) by speaking RESP directly over bash /dev/tcp.
# No redis-cli, no jq — runs identically on Linux and Git Bash on Windows.
#
# Modes (first arg):
#   hook        stdin = hook event JSON (SessionStart/UserPromptSubmit/Stop/SessionEnd)
#   statusline  stdin = statusline JSON; prints a one-line statusline to stdout
#
# Contract (see docs/plans/2026-07-02-001-feat-claude-mode-plan.md):
#   claude:session:<host>:<sid>  hash, TTL 2h; hooks own status/ts,
#                                statusline owns title/model and claude:limits.
#   claude:recent                LPUSH + LTRIM on SessionEnd (reason != clear).
#
# Fire-and-forget: network I/O is backgrounded, failures are silent, exit is
# always 0 — a dead Redis must never slow a Claude session down.

LC_ALL=C
export LC_ALL

KDD_REDIS_HOST="${KDD_REDIS_HOST:-192.168.1.144}"
KDD_REDIS_PORT="${KDD_REDIS_PORT:-6380}"
KDD_TTL_S=7200
KDD_RECENT_KEEP=19      # LTRIM 0 19 -> 20 entries
KDD_LIMITS_MIN_S=5      # statusline publish throttle

STATE_DIR="${HOME}/.claude/kdeskdash-pub/state"

# ---------- tiny JSON helpers (flat fields on a single-line document) ----------

# jstr <json> <field>: first "field":"value" occurrence, minimally unescaped.
jstr() {
  printf '%s' "$1" | sed -n 's/.*"'"$2"'"[[:space:]]*:[[:space:]]*"\(\(\\.\|[^"\\]\)*\)".*/\1/p' \
    | head -n1 | sed -e 's/\\"/"/g' -e 's/\\\\/\\/g' -e 's/\\\//\//g'
}

# jnum <json> <field>: first numeric field.
jnum() {
  printf '%s' "$1" | sed -n 's/.*"'"$2"'"[[:space:]]*:[[:space:]]*\(-\{0,1\}[0-9][0-9.]*\).*/\1/p' | head -n1
}

# sanitized token for key material (host/session id)
token() { printf '%s' "$1" | tr -cd 'A-Za-z0-9._-' | cut -c1-63; }

# ---------- RESP pipeline ----------

PAYLOAD=""
resp() {
  local a
  PAYLOAD+="*$#"$'\r\n'
  for a in "$@"; do
    PAYLOAD+="\$${#a}"$'\r\n'"${a}"$'\r\n'
  done
}

# Send the accumulated pipeline. Synchronous variant for events where the
# process is about to die (SessionEnd): a backgrounded sender loses the race
# with CLI exit / ssh teardown and the DEL never arrives (ghost session until
# TTL). The hook-level timeout bounds the worst case.
send_sync() {
  [ -n "$PAYLOAD" ] || return 0
  (
    exec 3<>"/dev/tcp/${KDD_REDIS_HOST}/${KDD_REDIS_PORT}" || exit 0
    printf '%s' "$PAYLOAD" >&3
    read -r -t 1 _ <&3   # give the server a beat to consume before close
    exec 3>&- 3<&-
  ) >/dev/null 2>&1
}

# Disowned fire-and-forget variant so live-session events never wait.
send() {
  [ -n "$PAYLOAD" ] || return 0
  send_sync &
  disown 2>/dev/null
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

# strip characters that would break the hand-built recent-record JSON
plain() { printf '%s' "$1" | tr -d '"\\\r\n\t' ; }

mkdir -p "$STATE_DIR" 2>/dev/null

# ---------- modes ----------

hook_mode() {
  local json event sid key cwd project reason started dur title rec
  json=$(cat)
  event=$(jstr "$json" hook_event_name)
  sid=$(token "$(jstr "$json" session_id)")
  [ -n "$sid" ] || exit 0
  cwd=$(jstr "$json" cwd)
  project=$(project_of "$cwd")
  key="claude:session:${HOST}:${sid}"

  case "$event" in
    SessionStart)
      printf '%s' "$NOW" > "${STATE_DIR}/${sid}.start" 2>/dev/null
      resp HSET "$key" host "$HOST" project "$project" cwd "$cwd" \
                 status working ts "$NOW" started_ts "$NOW"
      resp EXPIRE "$key" "$KDD_TTL_S"
      ;;
    UserPromptSubmit|Stop)
      local st=working
      [ "$event" = Stop ] && st=awaiting
      resp HSET "$key" host "$HOST" project "$project" cwd "$cwd" \
                 status "$st" ts "$NOW"
      resp EXPIRE "$key" "$KDD_TTL_S"
      ;;
    SessionEnd)
      reason=$(jstr "$json" reason)
      resp DEL "$key"
      if [ "$reason" != "clear" ]; then
        started=$(cat "${STATE_DIR}/${sid}.start" 2>/dev/null | tr -cd '0-9')
        dur=""
        [ -n "$started" ] && [ "$started" -le "$NOW" ] && dur=$((NOW - started))
        title=$(plain "$(cat "${STATE_DIR}/${sid}.title" 2>/dev/null)")
        rec="{\"host\":\"${HOST}\",\"project\":\"$(plain "$project")\",\"title\":\"${title}\",\"ended_ts\":${NOW},\"dur_s\":${dur:-0}}"
        resp LPUSH claude:recent "$rec"
        resp LTRIM claude:recent 0 "$KDD_RECENT_KEEP"
      fi
      rm -f "${STATE_DIR}/${sid}.start" "${STATE_DIR}/${sid}.title" 2>/dev/null
      find "$STATE_DIR" -type f -mtime +2 -delete 2>/dev/null
      send_sync   # the CLI is exiting; a backgrounded DEL would race and lose
      exit 0
      ;;
    *) exit 0 ;;
  esac
  send
}

statusline_mode() {
  local json sid key name model rl fh sd fh_pct fh_reset sd_pct sd_reset cw ctx line stamp
  json=$(cat)
  sid=$(token "$(jstr "$json" session_id)")
  name=$(jstr "$json" session_name)
  model=$(jstr "$json" display_name)   # model.display_name is the only display_name

  # context % for the local statusline text only; bound the segment before
  # rate_limits so its used_percentage fields can't be picked up instead
  cw="${json#*\"context_window\":}"
  cw="${cw%%\"rate_limits\"*}"
  ctx=""
  [ "$cw" != "$json" ] && ctx=$(jnum "$cw" used_percentage)

  # rate limits (absent for API-key auth)
  fh_pct="" ; sd_pct=""
  rl="${json#*\"rate_limits\":}"
  if [ "$rl" != "$json" ]; then
    fh="${rl#*\"five_hour\":\{}" ; fh="${fh%%\}*}"
    sd="${rl#*\"seven_day\":\{}" ; sd="${sd%%\}*}"
    fh_pct=$(jnum "$fh" used_percentage) ; fh_reset=$(jnum "$fh" resets_at)
    sd_pct=$(jnum "$sd" used_percentage) ; sd_reset=$(jnum "$sd" resets_at)
  fi

  # statusline text first — Redis must never delay it
  line="${model:-claude}"
  [ -n "$ctx" ]    && line="${line} · ctx ${ctx}%"
  [ -n "$fh_pct" ] && line="${line} · 5h ${fh_pct}%"
  [ -n "$sd_pct" ] && line="${line} · 7d ${sd_pct}%"
  printf '%s\n' "$line"

  # keep the title around for the SessionEnd recent-record
  [ -n "$sid" ] && [ -n "$name" ] && printf '%s' "$name" > "${STATE_DIR}/${sid}.title" 2>/dev/null

  # throttle the network side
  stamp=$(cat "${STATE_DIR}/limits.stamp" 2>/dev/null | tr -cd '0-9')
  [ -n "$stamp" ] && [ $((NOW - stamp)) -lt "$KDD_LIMITS_MIN_S" ] && exit 0
  printf '%s' "$NOW" > "${STATE_DIR}/limits.stamp" 2>/dev/null

  if [ -n "$fh_pct" ] || [ -n "$sd_pct" ]; then
    resp HSET claude:limits \
      five_hour_pct "${fh_pct:-0}" five_hour_resets_at "${fh_reset:-0}" \
      seven_day_pct "${sd_pct:-0}" seven_day_resets_at "${sd_reset:-0}" \
      updated_at "$NOW" host "$HOST"
  fi
  if [ -n "$sid" ]; then
    key="claude:session:${HOST}:${sid}"
    resp HSET "$key" title "$(plain "$name")" model "$(plain "$model")"
    resp EXPIRE "$key" "$KDD_TTL_S"   # never leave a TTL-less key behind
  fi
  send
}

case "$1" in
  hook)       hook_mode ;;
  statusline) statusline_mode ;;
esac
exit 0
