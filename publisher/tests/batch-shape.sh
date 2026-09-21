#!/usr/bin/env bash
# batch-shape.sh — what claude-pub.sh hands to kdash-pub, asserted exactly.
#
# The CD-7 port replaced a RESP pipeline (where every command is independent)
# with kdash-pub's tab-separated `batch` (where ONE off-contract line makes the
# whole batch publish nothing). That trade is worth it — the CLI brings khlenv,
# AUTH and the key grammar — but it means a malformed field is now a silent
# total loss for that event. This pins the format so it cannot drift.
#
# No network and no real kdash-pub: KDD_PUB_BIN points at a stub that records
# its argv and stdin, which is precisely the surface under test.

set -u
here=$(cd "$(dirname "$0")" && pwd)
script="$here/../claude-pub.sh"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

export KDD_STATE_DIR="$work/state"
export HOSTNAME=testhost
mkdir -p "$KDD_STATE_DIR"

# The stub. One file per invocation, so parallel legs never interleave.
cat > "$work/kdash-pub" <<'STUB'
#!/usr/bin/env bash
out=$(mktemp "$KDD_CAPTURE_DIR/leg.XXXXXX")
printf 'ARGV %s\n' "$*" > "$out"
# Only `batch` reads stdin, exactly as the real CLI does. A stub that drained
# stdin unconditionally would block forever on `hget`, whose stdin is whatever
# the hook happened to inherit.
case " $* " in *' batch '*) cat >> "$out" ;; esac
STUB
chmod +x "$work/kdash-pub"
export KDD_PUB_BIN="$work/kdash-pub"

fails=0
ok()   { printf 'ok   %s\n' "$1"; }
fail() { printf 'FAIL %s\n     %s\n' "$1" "$2"; fails=$((fails + 1)); }

# Run one event and collect every leg's capture, waiting for the backgrounded
# senders. Bounded: a hung sender must fail the test, not hang the gate.
run() {
  local mode="$1" legs="$2" want_legs="$3" stdin="$4" i
  export KDD_CAPTURE_DIR="$work/cap"
  rm -rf "$KDD_CAPTURE_DIR"; mkdir -p "$KDD_CAPTURE_DIR"
  # An empty `legs` runs with KDD_LEGS unset, which is the only way to test the
  # shipped default rather than a value this test supplied.
  if [ -n "$legs" ]; then
    printf '%s' "$stdin" | KDD_LEGS="$legs" "$script" "$mode" >/dev/null 2>&1
  else
    printf '%s' "$stdin" | "$script" "$mode" >/dev/null 2>&1
  fi
  for i in $(seq 1 100); do
    [ "$(find "$KDD_CAPTURE_DIR" -type f | wc -l)" -ge "$want_legs" ] && break
    sleep 0.05
  done
}

# Every batch body emitted, legs concatenated in a stable order.
bodies() { cat "$KDD_CAPTURE_DIR"/* 2>/dev/null | grep -v '^ARGV ' | sort; }
argvs()  { grep -h '^ARGV ' "$KDD_CAPTURE_DIR"/* 2>/dev/null | sort; }

expect() {
  local name="$1" want="$2" got="$3"
  if [ "$got" = "$want" ]; then ok "$name"
  else fail "$name" "want: $want
     got:  $got"; fi
}

TAB=$'\t'

# ---- 1. the SHIPPED DEFAULT is one authenticated leg on the claude stem ----
# Until kdashdata sprint 005 this defaulted to `interim,central` and wrote both
# homes through the CD-7 dual-write window. The interim home is retired, so a
# publisher still writing it would be feeding a Redis nobody reads. Run with
# KDD_LEGS unset on purpose: a default is the one thing a test that supplies the
# value cannot check.
run hook "" 1 \
  '{"hook_event_name":"SessionStart","session_id":"s1","cwd":"/home/ken/src/tools/kdeskdash"}'
expect "the shipped default is one authenticated leg on the claude stem" \
"ARGV --app kdeskdash --stem KDASH_CLAUDE_REDIS --best-effort batch" \
"$(argvs)"

expect "the default leg receives exactly one copy of the batch" \
"expire${TAB}claude:session:testhost:s1${TAB}7200
hset${TAB}claude:session:testhost:s1${TAB}host${TAB}testhost${TAB}project${TAB}kdeskdash${TAB}cwd${TAB}/home/ken/src/tools/kdeskdash${TAB}status${TAB}working${TAB}ts${TAB}TS${TAB}started_ts${TAB}TS" \
"$(bodies | sed -E 's/[0-9]{10}/TS/g')"

# ---- 2. the retired leg name publishes NOWHERE, rather than guessing ----
# `interim` used to be a real home. A stale caller (an un-upgraded unit, an old
# env file) naming it must write nothing at all — silently falling back to a
# live home would resurrect the dual-write this sprint exists to end.
run hook interim 0 \
  '{"hook_event_name":"Stop","session_id":"s1","cwd":"/tmp/proj"}'
sleep 0.2
expect "the retired 'interim' leg publishes nowhere" "0" \
"$(find "$KDD_CAPTURE_DIR" -type f | wc -l)"

# ---- 3. SessionEnd ships the DEL in its own batch ----
# A hand-built recent record that failed to be JSON would otherwise take the
# DEL down with it and leave a ghost session until the 2h TTL.
run hook central 2 \
  '{"hook_event_name":"SessionEnd","session_id":"s1","cwd":"/tmp/proj","reason":"exit"}'
expect "SessionEnd separates the DEL from the recent record" \
"del${TAB}claude:session:testhost:s1
lpush${TAB}claude:recent${TAB}{\"host\":\"testhost\",\"project\":\"proj\",\"title\":\"\",\"ended_ts\":TS,\"dur_s\":N}
ltrim${TAB}claude:recent${TAB}0${TAB}19" \
"$(bodies | sed -E 's/[0-9]{10}/TS/g; s/"dur_s":[0-9]+/"dur_s":N/')"
expect "the DEL batch is sent separately from the recent batch" "2" \
"$(find "$KDD_CAPTURE_DIR" -type f | wc -l)"

# ---- 4. a value carrying the delimiter cannot split a line ----
# `cwd` reaches the batch straight off the wire, unlike every field that goes
# through plain(). A LITERAL tab in the payload (malformed JSON, but the
# publisher does not get to choose what Claude Code hands it) would otherwise
# shift every following field by one and silently rewrite the record.
run hook central 1 \
  "$(printf '{"hook_event_name":"SessionStart","session_id":"s2","cwd":"/tmp/a\tb"}')"
expect "a literal tab inside a field is stripped, not emitted" \
"expire${TAB}claude:session:testhost:s2${TAB}7200
hset${TAB}claude:session:testhost:s2${TAB}host${TAB}testhost${TAB}project${TAB}ab${TAB}cwd${TAB}/tmp/ab${TAB}status${TAB}working${TAB}ts${TAB}TS${TAB}started_ts${TAB}TS" \
"$(bodies | sed -E 's/[0-9]{10}/TS/g')"

# ---- 5. lowercase verbs: kdash-pub's parser rejects HSET ----
run statusline central 1 \
  '{"session_id":"s3","session_name":"n","model":{"display_name":"Opus 5"},"rate_limits":{"five_hour":{"used_percentage":1},"seven_day":{"used_percentage":2}}}'
if bodies | grep -qE '^[A-Z]'; then
  fail "every verb is lowercase" "$(bodies | grep -E '^[A-Z]')"
else
  ok "every verb is lowercase"
fi

# ---- 6. a host with no kdash-pub leaves a findable breadcrumb ----
# The override is checked for executability too, so this covers both a host
# the store never reached and an override naming a file that is not there.
rm -rf "$work/cap"; mkdir -p "$work/cap"
printf '{"hook_event_name":"SessionStart","session_id":"s4","cwd":"/tmp/p"}' \
  | KDD_PUB_BIN="$work/not-installed" KDD_LEGS=central "$script" hook >/dev/null 2>&1
if [ -s "$KDD_STATE_DIR/no-kdash-pub" ]; then
  ok "a missing kdash-pub leaves a breadcrumb"
else
  fail "a missing kdash-pub leaves a breadcrumb" "no $KDD_STATE_DIR/no-kdash-pub"
fi

# ---- 7. poll's freshness guard reads through kdash-pub, on the leg it writes -
# The guard stops a poll writer publishing over a fresher observation from a
# live statusline on any host. It used to be a raw /dev/tcp RESP request aimed
# at the unauthenticated interim home; that home is gone, so it now goes through
# the CLI's read verb (kdashdata CD-14) on the SAME stem the write will use — a
# guard reading a different Redis than the one it overwrites is worse than none.
appdata="$work/appdata"
mkdir -p "$appdata/Claude"
printf '{"samples":[{"t":%s000,"u":{"fh":7,"sd":3}}]}' "$(date +%s)" \
  > "$appdata/Claude/plan-usage-history.json"
export KDD_CAPTURE_DIR="$work/cap"
rm -rf "$KDD_CAPTURE_DIR"; mkdir -p "$KDD_CAPTURE_DIR"
APPDATA="$appdata" "$script" poll </dev/null >/dev/null 2>&1
expect "poll guards its write with an hget on the same stem it publishes to" \
"ARGV --app kdeskdash --stem KDASH_CLAUDE_REDIS --best-effort batch
ARGV --app kdeskdash --stem KDASH_CLAUDE_REDIS --best-effort hget claude:limits scoped_updated_at
ARGV --app kdeskdash --stem KDASH_CLAUDE_REDIS --best-effort hget claude:limits updated_at" \
"$(argvs)"

# ---- 8. no hand-rolled sockets remain ----
# The whole point of routing through kdash-pub is that khlenv, AUTH and the key
# grammar are not optional. A /dev/tcp anywhere in this script is a path around
# all three, and it is how the last one survived a cutover.
if grep -q 'dev/tcp' "$script" && grep -v '^[[:space:]]*#' "$script" | grep -q 'dev/tcp'; then
  fail "no hand-rolled sockets remain" "$(grep -n 'dev/tcp' "$script" | grep -v ':[[:space:]]*#')"
else
  ok "no hand-rolled sockets remain"
fi

# A substring assertion, for the cases below where two fields in one batch are
# both 10-digit epochs and the blanket TS normalisation would erase the very
# difference under test.
has() {
  if bodies | grep -qF -- "$2"; then ok "$1"
  else fail "$1" "missing: $2
     got:  $(bodies)"; fi
}

# A transcript that has already carried a turn, and one that has not. Written
# here rather than committed as fixtures: what is under test is two lines of
# shape, and a fixture file would invite it being read as a real transcript.
tr_used="$work/used.jsonl"
printf '%s\n' \
  '{"type":"user","timestamp":"2026-09-01T10:00:00.000Z"}' \
  '{"type":"assistant","timestamp":"2026-09-01T10:00:05.000Z"}' > "$tr_used"
tr_new="$work/new.jsonl"
printf '%s\n' '{"type":"user","timestamp":"2026-09-01T10:00:00.000Z"}' > "$tr_new"

# ---- 9. a RE-ATTACH does not latch `working`, and keeps the real start ----
# The desktop app re-opening a finished session fires a genuine SessionStart
# carrying the old uuid, so `source` cannot tell it from a cold start (WI 2719).
# Publishing `working` there pinned two finished sessions on the panel for the
# full 2h TTL. The transcript is the discriminator that is actually available.
printf '1756720800' > "$KDD_STATE_DIR/s5.start"
run hook central 1 \
  "{\"hook_event_name\":\"SessionStart\",\"session_id\":\"s5\",\"cwd\":\"/tmp/proj\",\"transcript_path\":\"$tr_used\"}"
has "a re-attached session publishes awaiting, not working" "status${TAB}awaiting"
has "a re-attached session keeps its original started_ts" "started_ts${TAB}1756720800"

# ---- 10. a genuinely new session is unchanged ----
# The narrow fix (the WI's option 2): `working` still means working on a cold
# start, so no consumer's display ladder changes.
run hook central 1 \
  "{\"hook_event_name\":\"SessionStart\",\"session_id\":\"s6\",\"cwd\":\"/tmp/proj\",\"transcript_path\":\"$tr_new\"}"
has "a new session still publishes working" "status${TAB}working"

# ---- 11. a re-attach whose .start was swept still reports a real start ----
# STATE_DIR is swept at two days, which is exactly the age of a session worth
# re-attaching. Falling back to NOW would put the bogus dur_s back on
# claude:recent by another route, so the transcript's first stamp is used.
want_start=$(date -u -d '2026-09-01T10:00:00.000Z' +%s 2>/dev/null)
run hook central 1 \
  "{\"hook_event_name\":\"SessionStart\",\"session_id\":\"s7\",\"cwd\":\"/tmp/proj\",\"transcript_path\":\"$tr_used\"}"
has "a swept .start falls back to the transcript's first stamp" \
  "started_ts${TAB}${want_start}"

# ---- 12. Stop is delivered before the hook returns ----
# In print mode the CLI fires SessionEnd ~15 ms after Stop and exits. A
# backgrounded Stop sender loses that race two ways (WI 2809): torn down with
# the process before it publishes, or landing AFTER SessionEnd's DEL and
# re-creating the row that DEL just closed. Measured: 3 of 3 headless sessions
# left `working` before this, 6 of 6 deleted cleanly after.
#
# No poll and no sleep here on purpose — that IS the assertion. A backgrounded
# sender makes this flaky-to-failing, which is the regression to catch.
export KDD_CAPTURE_DIR="$work/cap"
rm -rf "$KDD_CAPTURE_DIR"; mkdir -p "$KDD_CAPTURE_DIR"
printf '%s' '{"hook_event_name":"Stop","session_id":"s8","cwd":"/tmp/proj"}' \
  | KDD_LEGS=central "$script" hook >/dev/null 2>&1
expect "Stop's batch is delivered before the hook returns" "1" \
"$(find "$KDD_CAPTURE_DIR" -type f | wc -l)"
has "Stop still publishes awaiting" "status${TAB}awaiting"

[ "$fails" -eq 0 ] || { printf '\n%d assertion(s) failed\n' "$fails"; exit 1; }
printf '\nall batch-shape assertions passed\n'
