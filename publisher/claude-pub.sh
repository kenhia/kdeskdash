#!/bin/bash
# claude-pub.sh — kdeskdash claude-feed publisher.
#
# Publishes Claude Code session activity and subscription usage limits through
# `kdash-pub`, kdashdata's publisher CLI: khlenv-resolved endpoint, CD-12 auth,
# and the key grammar checked before anything reaches Redis. No redis-cli, no
# jq — runs identically on Linux and Git Bash on Windows.
#
# CD-7 is complete: `KDASH_CLAUDE_REDIS` names the central Redis (rpi53:6379,
# authenticated), and the interim home no longer carries this feed. One leg, one
# home. See KDD_LEGS below.
#
# Modes (first arg):
#   hook        stdin = hook event JSON (SessionStart/UserPromptSubmit/Stop/
#               SessionEnd, plus PreToolUse/PostToolUse on EVERY tool: the
#               AskUserQuestion pair drives `blocked`, and every other tool is
#               a throttled ts-only keepalive)
#   statusline  stdin = statusline JSON; prints a one-line statusline to stdout
#   poll        no stdin; refreshes claude:limits with NO session running
#
# Contract (see sprints/007-claude-mode/plan.md):
#   claude:session:<host>:<sid>  hash, TTL 2h; hooks own status/ts (+ model and
#                                title, both read from the transcript);
#                                statusline owns claude:limits and re-writes an
#                                agreeing title/model (TUI sessions only).
#   claude:recent                LPUSH + LTRIM on SessionEnd (reason != clear).
#
# claude:limits gained a `source` field (statusline|file|oauth) and `updated_at`
# is now the OBSERVATION time, not the publish time — poll mode refuses to write
# over a newer observation, so a live statusline (on any host) always wins.
#
# Fire-and-forget: network I/O is backgrounded, failures are silent, exit is
# always 0 — a dead Redis must never slow a Claude session down.

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

# Which homes this publisher writes, comma-separated (CD-7):
#   claude   --stem KDASH_CLAUDE_REDIS   the feed's home; rpi53:6379 since the flip
#   central  --stem KDASH_CENTRAL_REDIS  the same Redis under the fleet-wide stem
# Both resolve to rpi53:6379 today and are kept apart deliberately: the claude
# family keeps its own address so it can move again without touching this file.
# The `interim` leg (rpidash2:6380, unauthenticated) was retired with the old
# home in kdashdata sprint 005 — a publisher still writing it would be feeding
# a Redis nobody reads.
KDD_LEGS="${KDD_LEGS:-claude}"

KDD_TTL_S=7200
KDD_RECENT_KEEP=19      # LTRIM 0 19 -> 20 entries
KDD_LIMITS_MIN_S=5      # statusline publish throttle
KDD_HEARTBEAT_MIN_S=120 # tool-use keepalive throttle (panel greys at 15m)
KDD_POLL_MAX_AGE_S=900  # plan-usage-history sample older than this = app closed

# What the panel should expect between writes from each mode, published as
# expected_refresh_s / scoped_expected_refresh_s so the greying policy lives
# with the thing that knows its own cadence (the panel adds its own buffer).
KDD_STATUSLINE_EXPECT_S=60  # statusline: sub-minute while a session renders
KDD_POLL_EXPECT_S=300       # poll: the 5-minute timer on every host

# Overridable so the batch-shape test can run without touching a real
# install's throttle state (and vice versa).
STATE_DIR="${KDD_STATE_DIR:-${HOME}/.claude/kdeskdash-pub/state}"

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

# ---------- transport: kdash-pub batch ----------
#
# Commands accumulate as tab-separated lines — kdash-pub's `batch` format, which
# needs no quoting rules because a JSON string cannot contain a literal tab —
# and go out in one connection per home. `--best-effort` maps a delivery failure
# to exit 0, because a dead Redis must never fail a hook; an off-contract key
# still exits 1, which is a bug worth noticing.

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
#
# One mapping, used by the write path AND by the poll guard's read below — the
# guard exists to compare against what the write is about to overwrite, so the
# two resolving to different Redises would make it worse than useless. Sets a
# variable rather than printing: send_leg is on the every-tool-call path and a
# command substitution there is a fork.
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
# die (SessionEnd): a backgrounded sender loses the race with CLI exit / ssh
# teardown and the DEL never arrives (ghost session until TTL). The hook-level
# timeout bounds the worst case; kdash-pub's own is 1.5 s per leg.
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

# strip characters that would break the hand-built recent-record JSON
plain() { printf '%s' "$1" | tr -d '"\\\r\n\t' ; }

mkdir -p "$STATE_DIR" 2>/dev/null

# ---------- modes ----------

# Friendly short label from a model id: claude-opus-4-8 -> "Opus 4.8". Session
# hooks fire everywhere (desktop app, headless, TUI); the statusline does not,
# so model has to come from the transcript for most sessions.
model_label() {
  local id fam ver
  id="${1#claude-}"
  case "$id" in
    *-[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]) id="${id%-????????}" ;;  # drop -YYYYMMDD
  esac
  fam="${id%%-*}"
  ver="${id#*-}"
  [ "$ver" = "$id" ] && ver=""      # no version tokens
  ver="${ver//-/.}"                 # 4-8 -> 4.8
  [ -n "$fam" ] || { printf 'claude'; return; }
  fam="$(printf '%s' "${fam:0:1}" | tr a-z A-Z)${fam:1}"
  if [ -n "$ver" ]; then printf '%s %s' "$fam" "$ver"; else printf '%s' "$fam"; fi
}

# Last main-model id from a session transcript (works where statusline never
# runs). Bounded tail keeps the cost independent of transcript size.
model_from_transcript() {
  local tpath="$1"
  tpath=$(printf '%s' "$tpath" | tr '\\' '/')  # Windows backslashes -> forward slashes
  [ -n "$tpath" ] && [ -f "$tpath" ] || return 0
  tail -n 80 "$tpath" 2>/dev/null \
    | grep -o '"model":"claude-[A-Za-z0-9._-]*"' \
    | tail -n1 | sed -e 's/.*:"//' -e 's/"$//'
}

# Claude's own session name, e.g. "Build Honorverse star system data scraper".
# It is in NO hook or statusline payload — the hook `session_title` field only
# ever carries a user-set --name//rename. The auto-name lives only in the
# transcript, as one of two record types:
#   {"type":"ai-title","aiTitle":"…"}          CLI / Code sessions
#   {"type":"custom-title","customTitle":"…"}  desktop app auto-name, CLI rename
# Both are rewritten as the session evolves, so the LAST record of either type
# is the current one (measured 2026-07-27 over 55 transcripts: median 3, p90 15,
# max 24 lines from EOF, and only 1/55 carried both — last-in-file settles it).
title_from_transcript() {
  local tpath="$1"
  tpath=$(printf '%s' "$tpath" | tr '\\' '/')
  [ -n "$tpath" ] && [ -f "$tpath" ] || return 0
  tail -n 100 "$tpath" 2>/dev/null \
    | grep -oE '"(aiTitle|customTitle)":"([^"\\]|\\.)*"' \
    | tail -n1 \
    | sed -e 's/^"[A-Za-z]*"[[:space:]]*:[[:space:]]*"//' -e 's/"$//' \
          -e 's/\\"/"/g' -e 's/\\\\/\\/g' -e 's/\\\//\//g'
}

# Has this transcript already carried a turn? The discriminator SessionStart's
# own payload cannot supply (WI 2719): the desktop app re-opening an old session
# spawns a FRESH CLI with the old uuid and no --resume, so `source` is "startup"
# for a re-attach exactly as it is for a new session — and the emit site is
# skipped on a real --resume, so `source` can never carry the distinction.
#
# grep exits at the first match, so a re-attached transcript costs the bytes up
# to its first assistant record and a genuinely new one costs the whole file —
# which is a few KB, because it is new. Once per session, never on the hot path.
has_turns() {
  local t="$1"
  t=$(printf '%s' "$t" | tr '\\' '/')
  [ -n "$t" ] && [ -f "$t" ] || return 1
  grep -qE '"type":"assistant"' "$t" 2>/dev/null
}

# Epoch of the transcript's first timestamped record — the session's real start,
# used when the local .start file is gone (the STATE_DIR sweep deletes it after
# two days, and a re-attach of something older is precisely the case this is
# for). Prints nothing if the transcript has no parseable stamp; the caller
# falls back to NOW rather than publishing a guess.
first_turn_ts() {
  local t="$1" iso
  t=$(printf '%s' "$t" | tr '\\' '/')
  [ -n "$t" ] && [ -f "$t" ] || return 0
  iso=$(grep -oE '"timestamp":"[0-9]{4}-[^"]*"' "$t" 2>/dev/null | head -n1 | cut -d'"' -f4)
  [ -n "$iso" ] || return 0
  date -u -d "$iso" +%s 2>/dev/null
}

# Keepalive for a session that is working but not emitting lifecycle events.
# Between UserPromptSubmit and Stop a long turn publishes nothing at all, so
# `ts` sits at prompt-submit time and the panel greys the row to IDLE at
# CF_IDLE_S (15m, src/claude_feed.h) while the agent is still cranking.
#
# Refreshes `ts` ONLY, never `status`. A backgrounded tool call or a subagent
# must not be able to downgrade a `blocked` row — an agent sitting on an
# AskUserQuestion — to `working`; this write can make a row fresher, but it can
# never change what the row claims. cf_session_from_fields wants status+ts in
# the HASH, not in the write, so a bare ts update against a live key parses.
#
# Throttled per session, same shape as limits.stamp: at a 2-minute cadence
# against a 15-minute threshold, seven writes cover a window and a single
# dropped one can never grey a row.
heartbeat() {
  local sid="$1" key="$2" hb
  hb=$(cat "${STATE_DIR}/${sid}.hb" 2>/dev/null | tr -cd '0-9')
  [ -n "$hb" ] && [ $((NOW - hb)) -lt "$KDD_HEARTBEAT_MIN_S" ] && return 0
  printf '%s' "$NOW" > "${STATE_DIR}/${sid}.hb" 2>/dev/null
  cmd hset "$key" ts "$NOW"
  cmd expire "$key" "$KDD_TTL_S"   # never leave a TTL-less key behind
  send
}

hook_mode() {
  local json event sid key cwd project reason started dur title rec tpath mid sname
  json=$(cat)
  event=$(jstr "$json" hook_event_name)
  sid=$(token "$(jstr "$json" session_id)")
  [ -n "$sid" ] || exit 0
  key="claude:session:${HOST}:${sid}"

  # Keepalive fast path, taken before any further parsing. PreToolUse and
  # PostToolUse are matched on every tool now, which makes this the hottest
  # path in the script — once per tool call in every session on the box — so it
  # pays for only the three fields it actually needs and skips the transcript
  # enrichment below entirely. AskUserQuestion is the exception and falls
  # through to the blocked/working logic.
  case "$event" in
    PreToolUse|PostToolUse)
      if [ "$(jstr "$json" tool_name)" != AskUserQuestion ]; then
        heartbeat "$sid" "$key"
        exit 0
      fi
      ;;
  esac

  cwd=$(jstr "$json" cwd)
  project=$(project_of "$cwd")
  tpath=$(jstr "$json" transcript_path)

  case "$event" in
    SessionStart)
      # A SessionStart whose transcript already has turns is a RE-ATTACH, and
      # nothing has been asked of it — `awaiting` is the honest value, where
      # `working` latches a finished session for the full 2h TTL and re-arms
      # every time it is re-opened (WI 2719). `awaiting` is a status the feed
      # already publishes at Stop, so no consumer learns a new word for this.
      #
      # started_ts must survive the re-attach too: overwriting it made session
      # age wrong and had SessionEnd compute dur_s from the re-attach, pushing
      # a bogus short entry onto claude:recent.
      local st=working sts="$NOW"
      if has_turns "$tpath"; then
        st=awaiting
        sts=$(cat "${STATE_DIR}/${sid}.start" 2>/dev/null | tr -cd '0-9')
        [ -n "$sts" ] || sts=$(first_turn_ts "$tpath")
        [ -n "$sts" ] || sts="$NOW"
      fi
      printf '%s' "$sts" > "${STATE_DIR}/${sid}.start" 2>/dev/null
      cmd hset "$key" host "$HOST" project "$project" cwd "$cwd" \
                 status "$st" ts "$NOW" started_ts "$sts"
      cmd expire "$key" "$KDD_TTL_S"
      ;;
    UserPromptSubmit|Stop)
      local st=working
      [ "$event" = Stop ] && st=awaiting
      cmd hset "$key" host "$HOST" project "$project" cwd "$cwd" \
                 status "$st" ts "$NOW"
      cmd expire "$key" "$KDD_TTL_S"
      ;;
    PreToolUse|PostToolUse)
      # Only AskUserQuestion reaches here — the fast path above routed every
      # other tool to the keepalive. It means the agent is hard-blocked on the
      # user: PreToolUse fires before the dialog is shown, PostToolUse once it
      # is answered.
      #
      # The payload also carries the question text and the user's answer. Read
      # nothing but tool_name — prompt content never reaches Redis (R20).
      local qst=blocked
      [ "$event" = PostToolUse ] && qst=working
      cmd hset "$key" host "$HOST" project "$project" cwd "$cwd" \
                 status "$qst" ts "$NOW"
      cmd expire "$key" "$KDD_TTL_S"
      ;;
    SessionEnd)
      reason=$(jstr "$json" reason)
      # The DEL ships in its OWN batch, before the recent record. kdash-pub
      # refuses a whole batch when any line is off-contract, so a hand-built
      # record that somehow failed to be JSON would otherwise take the DEL down
      # with it and leave a ghost session until the 2h TTL.
      cmd del "$key"
      send_sync
      if [ "$reason" != "clear" ]; then
        started=$(cat "${STATE_DIR}/${sid}.start" 2>/dev/null | tr -cd '0-9')
        dur=""
        [ -n "$started" ] && [ "$started" -le "$NOW" ] && dur=$((NOW - started))
        title=$(plain "$(cat "${STATE_DIR}/${sid}.title" 2>/dev/null)")
        rec="{\"host\":\"${HOST}\",\"project\":\"$(plain "$project")\",\"title\":\"${title}\",\"ended_ts\":${NOW},\"dur_s\":${dur:-0}}"
        cmd lpush claude:recent "$rec"
        cmd ltrim claude:recent 0 "$KDD_RECENT_KEEP"
      fi
      rm -f "${STATE_DIR}/${sid}.start" "${STATE_DIR}/${sid}.title" \
            "${STATE_DIR}/${sid}.hb" 2>/dev/null
      find "$STATE_DIR" -type f -mtime +2 -delete 2>/dev/null
      send_sync   # the CLI is exiting; a backgrounded DEL would race and lose
      exit 0
      ;;
    *) exit 0 ;;
  esac
  # Enrich with model + session name from the transcript — the only source hooks
  # can see (the statusline, which also writes both, never runs headless/desktop
  # and so covers only a fraction of the fleet). The name lags: Claude does not
  # generate one for the first few turns, so `title` stays empty early and the
  # view falls back to `project`.
  mid=$(model_from_transcript "$tpath")
  [ -n "$mid" ] && cmd hset "$key" model "$(plain "$(model_label "$mid")")"
  sname=$(title_from_transcript "$tpath")
  if [ -n "$sname" ]; then
    cmd hset "$key" title "$(plain "$sname")"
    printf '%s' "$sname" > "${STATE_DIR}/${sid}.title" 2>/dev/null
  fi
  # Stop is the last event of a turn, and in print mode the CLI fires SessionEnd
  # ~15 ms later and exits. A backgrounded sender loses that race two ways, both
  # measured in WI 2809: the disowned child is torn down with the process before
  # it publishes at all, and when it does survive it can land AFTER SessionEnd's
  # DEL and re-create the row that DEL just closed — which is how a finished
  # headless leg came to read `working` for the full 2h TTL. Same reason
  # SessionEnd is synchronous. One write per turn end; the cost is milliseconds,
  # and it is paid where nothing is waiting on it.
  case "$event" in
    Stop) send_sync ;;
    *)    send ;;
  esac
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
    cmd hset claude:limits \
      five_hour_pct "${fh_pct:-0}" five_hour_resets_at "${fh_reset:-0}" \
      seven_day_pct "${sd_pct:-0}" seven_day_resets_at "${sd_reset:-0}" \
      updated_at "$NOW" host "$HOST" source statusline \
      expected_refresh_s "$KDD_STATUSLINE_EXPECT_S"
  fi
  if [ -n "$sid" ]; then
    key="claude:session:${HOST}:${sid}"
    cmd hset "$key" title "$(plain "$name")" model "$(plain "$model")"
    cmd expire "$key" "$KDD_TTL_S"   # never leave a TTL-less key behind
  fi
  send
}

# ---------- poll mode: limits with no session running ----------
#
# The statusline is the only thing that ever fed claude:limits, so the panel
# froze the moment Ken stopped working. Two session-free sources, best first:
#
#   file   %APPDATA%/Claude/plan-usage-history.json — the DESKTOP APP writes it
#          itself on a 5-minute timer (measured: 4638 of 4660 gaps were exactly
#          5 min over 27 days). Needs the app open, not a session. No network,
#          no credentials, nothing to get wrong. Percentages ONLY — the file
#          carries no reset timestamps.
#   oauth  GET api.anthropic.com/api/oauth/usage, the same call the official
#          client makes. For headless hosts with no desktop app. Undocumented,
#          so every failure here is "keep the last value", never "publish 0".
#
# Results land in these globals rather than a parsed return string.
P_T="" ; P_FH="" ; P_SD="" ; P_FHR="" ; P_SDR="" ; P_SRC=""
# Model-scoped weekly window (oauth only): model label, percent, resets epoch,
# is_active flag, and how many weekly_scoped entries the reply carried.
P_SCM="" ; P_SCP="" ; P_SCR="" ; P_SCA="" ; P_SCN=""

# Stored observation time (updated_at / scoped_updated_at), so a writer can
# refuse to publish over a fresher one. Empty when absent or unreachable.
#
# This was the last hand-rolled RESP request in this script — a raw /dev/tcp
# socket aimed at the unauthenticated interim home, which was the only endpoint
# bash could reach without re-deriving kdash-pub's auth rules. That home is gone
# and the stem is authenticated now, so the read goes through the CLI's own read
# verb (kdashdata CD-14).
#
# It reads the FIRST leg — the home the write it guards will reach — because a
# guard comparing against a different Redis than the one it is about to
# overwrite is worse than no guard at all.
#
# Failure stays benign by design: `--best-effort` maps an unreachable Redis to
# exit 0 with no output, an absent field prints nothing either, and both mean
# "unknown" here, on which the guard publishes.
stored_epoch() {
  [ -n "$KDD_PUB_BIN" ] || return 0
  leg_stem "${KDD_LEGS%%,*}" || return 0
  "$KDD_PUB_BIN" --app kdeskdash --stem "$LEG_STEM" --best-effort \
    hget claude:limits "$1" 2>/dev/null | tr -cd '0-9'
}

# ISO-8601 (with fractional seconds and offset) -> epoch seconds. GNU date only;
# a BSD/macOS date fails silently to empty, which the caller treats as unknown.
iso2epoch() {
  [ -n "$1" ] || return 0
  date -d "$1" +%s 2>/dev/null | tr -cd '0-9'
}

from_file() {
  local f rec t age
  for f in "${APPDATA}/Claude/plan-usage-history.json" \
           "${LOCALAPPDATA}/Packages/Claude_pzs8sxrjxfjjc/LocalCache/Roaming/Claude/plan-usage-history.json" \
           "${HOME}/Library/Application Support/Claude/plan-usage-history.json" \
           "${HOME}/.config/Claude/plan-usage-history.json" ; do
    [ -n "$f" ] && [ -f "$f" ] || continue
    # One 400KB line; only the tail matters. Match a WHOLE sample object so a
    # half-flushed write can never pair this sample's t with the last one's fh.
    rec=$(tail -c 600 "$f" 2>/dev/null | tr -d '\r\n' \
          | grep -oE '\{"t":[0-9]+,[^{}]*"u":\{[^{}]*\}\}' | tail -n1)
    [ -n "$rec" ] || continue
    t=$(jnum "$rec" t)
    [ -n "$t" ] || continue
    t=$((t / 1000))
    age=$((NOW - t)) ; [ "$age" -lt 0 ] && age=0
    [ "$age" -le "$KDD_POLL_MAX_AGE_S" ] || continue   # app closed; try oauth
    P_FH=$(jnum "$rec" fh) ; P_SD=$(jnum "$rec" sd)
    [ -n "$P_FH" ] || [ -n "$P_SD" ] || continue
    P_T="$t" ; P_SRC=file
    return 0
  done
  return 1
}

# Cached `claude --version`, refreshed daily. The UA below is load-bearing: a
# request without claude-code/<version> lands in an aggressively rate-limited
# bucket and gets persistent 429s (anthropics/claude-code#31021).
cli_version() {
  local vf="${STATE_DIR}/cli.version" v mt c
  if [ -f "$vf" ]; then
    v=$(tr -cd '0-9.' < "$vf" 2>/dev/null)
    mt=$(stat -c %Y "$vf" 2>/dev/null | tr -cd '0-9')
    [ -n "$v" ] && [ -n "$mt" ] && [ $((NOW - mt)) -lt 86400 ] && { printf '%s' "$v"; return 0; }
  fi
  for c in claude "${HOME}/.local/bin/claude" "${HOME}/.claude/local/claude" /usr/local/bin/claude ; do
    v=$("$c" --version 2>/dev/null | awk '{print $1}' | tr -cd '0-9.')
    [ -n "$v" ] && { printf '%s' "$v" > "$vf" 2>/dev/null; printf '%s' "$v"; return 0; }
  done
  # Stale cache beats no UA; no cache at all means we must not call.
  [ -n "$v" ] && printf '%s' "$v"
  return 0
}

# accessToken from the CLI's own credential file. Bounded to the claudeAiOauth
# block: mcpOAuth entries further down the file carry accessToken fields too,
# and jstr's greedy match would otherwise return the LAST one.
oauth_token() {
  local f="${HOME}/.claude/.credentials.json" j exp
  [ -f "$f" ] || return 1
  j=$(tr -d '\r\n' < "$f" 2>/dev/null)
  case "$j" in *'"claudeAiOauth"'*) ;; *) return 1 ;; esac
  j="${j#*\"claudeAiOauth\"}"
  j="${j%%\"mcpOAuth\"*}"
  exp=$(jnum "$j" expiresAt)                       # ms; note refreshTokenExpiresAt
  [ -n "$exp" ] && [ $((exp / 1000)) -le "$NOW" ] && return 1   # expired
  case "$j" in *'"user:profile"'*) ;; *) return 1 ;; esac        # scope required
  jstr "$j" accessToken
}

from_oauth() {
  local tok ver body fhb sdb lim seg
  command -v curl >/dev/null 2>&1 || return 1
  tok=$(oauth_token) || return 1
  [ -n "$tok" ] || return 1
  ver=$(cli_version)
  [ -n "$ver" ] || return 1
  body=$(curl -s --max-time 10 https://api.anthropic.com/api/oauth/usage \
           -H "Authorization: Bearer ${tok}" \
           -H "anthropic-beta: oauth-2025-04-20" \
           -H "User-Agent: claude-code/${ver}" 2>/dev/null | tr -d '\r\n')
  [ -n "$body" ] || return 1
  # "seven_day":{ matches the exact key; seven_day_opus & friends are :null.
  fhb="${body#*\"five_hour\":\{}" ; fhb="${fhb%%\}*}"
  sdb="${body#*\"seven_day\":\{}" ; sdb="${sdb%%\}*}"
  [ "$fhb" != "$body" ] || return 1
  P_FH=$(jnum "$fhb" utilization) ; P_FH="${P_FH%%.*}"   # 22.0 -> 22
  P_SD=$(jnum "$sdb" utilization) ; P_SD="${P_SD%%.*}"
  [ -n "$P_FH" ] || [ -n "$P_SD" ] || return 1
  P_FHR=$(iso2epoch "$(jstr "$fhb" resets_at)")
  P_SDR=$(iso2epoch "$(jstr "$sdb" resets_at)")
  # The model-scoped weekly window rides only in limits[] — the legacy
  # top-level keys (seven_day_opus & friends) are :null and never carry it.
  # Bound to the array, then to the first weekly_scoped entry; only one has
  # ever been observed, and scoped_count records if that changes. The model
  # arrives as a display string with a null id ("Fable") — pass it through,
  # never match on it.
  lim="${body#*\"limits\":\[}"
  if [ "$lim" != "$body" ]; then
    lim="${lim%%\]*}"
    P_SCN=$(printf '%s' "$lim" | grep -o '"kind":"weekly_scoped"' | wc -l | tr -cd '0-9')
    seg="${lim#*\"kind\":\"weekly_scoped\"}"
    if [ "$seg" != "$lim" ]; then
      seg="${seg%%\"kind\":*}"          # bound to this entry, not the next
      P_SCP=$(jnum "$seg" percent) ; P_SCP="${P_SCP%%.*}"
      P_SCM=$(jstr "$seg" display_name)
      P_SCR=$(iso2epoch "$(jstr "$seg" resets_at)")
      P_SCA=0 ; case "$seg" in *'"is_active":true'*) P_SCA=1 ;; esac
    fi
  fi
  P_T="$NOW" ; P_SRC=oauth
  return 0
}

poll_mode() {
  local prev sprev
  from_file || from_oauth || exit 0

  # The headline and scoped sets are guarded INDEPENDENTLY, each by its own
  # observation stamp. Sharing one guard would let a fresher file-source write
  # (which cannot supply scoped fields) block an oauth writer's scoped update —
  # the exact silent-freeze the split stamps exist to prevent.
  prev=$(stored_epoch updated_at)
  sprev=$(stored_epoch scoped_updated_at)

  BATCH=""
  # Headline: never publish over a fresher observation — a live statusline,
  # on this host or any other, is always the better number.
  if [ -z "$prev" ] || [ "$prev" -lt "${P_T:-$NOW}" ]; then
    cmd hset claude:limits \
      five_hour_pct "${P_FH:-0}" seven_day_pct "${P_SD:-0}" \
      updated_at "${P_T:-$NOW}" host "$HOST" source "$P_SRC" \
      expected_refresh_s "$KDD_POLL_EXPECT_S"
    # Reset stamps only when the source actually knows them. The history file
    # carries percentages only, and writing 0 would break the panel's countdown —
    # leaving the previous value in place is the honest degradation.
    [ -n "$P_FHR" ] && cmd hset claude:limits five_hour_resets_at "$P_FHR"
    [ -n "$P_SDR" ] && cmd hset claude:limits seven_day_resets_at "$P_SDR"
  fi
  # Scoped set: oauth is its only producer, stamped with its own
  # scoped_updated_at so a headline-only write can never make it look fresh.
  if [ -n "$P_SCM" ] && [ -n "$P_SCP" ] && \
     { [ -z "$sprev" ] || [ "$sprev" -lt "${P_T:-$NOW}" ]; }; then
    cmd hset claude:limits \
      scoped_model "$(plain "$P_SCM")" scoped_pct "$P_SCP" \
      scoped_active "${P_SCA:-0}" scoped_count "${P_SCN:-1}" \
      scoped_updated_at "${P_T:-$NOW}" \
      scoped_expected_refresh_s "$KDD_POLL_EXPECT_S"
    [ -n "$P_SCR" ] && cmd hset claude:limits scoped_resets_at "$P_SCR"
  fi
  send_sync
}

case "$1" in
  hook)       hook_mode ;;
  statusline) statusline_mode ;;
  poll)       poll_mode ;;
esac
exit 0
