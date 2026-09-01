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
cat >> "$out"
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
  printf '%s' "$stdin" | KDD_LEGS="$legs" "$script" "$mode" >/dev/null 2>&1
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

# ---- 1. the two legs of the dual-write window, and their flags ----
run hook interim,central 2 \
  '{"hook_event_name":"SessionStart","session_id":"s1","cwd":"/home/ken/src/tools/kdeskdash"}'
expect "dual-write legs carry the right stems and flags" \
"ARGV --app kdeskdash --stem KDASH_CENTRAL_REDIS --best-effort batch
ARGV --app kdeskdash --stem KDASH_CLAUDE_REDIS --no-auth --best-effort batch" \
"$(argvs)"

# Both legs must receive the SAME batch — a divergence would put the two homes
# out of step for the whole window.
expect "both legs receive an identical batch" \
"expire${TAB}claude:session:testhost:s1${TAB}7200
expire${TAB}claude:session:testhost:s1${TAB}7200
hset${TAB}claude:session:testhost:s1${TAB}host${TAB}testhost${TAB}project${TAB}kdeskdash${TAB}cwd${TAB}/home/ken/src/tools/kdeskdash${TAB}status${TAB}working${TAB}ts${TAB}TS${TAB}started_ts${TAB}TS
hset${TAB}claude:session:testhost:s1${TAB}host${TAB}testhost${TAB}project${TAB}kdeskdash${TAB}cwd${TAB}/home/ken/src/tools/kdeskdash${TAB}status${TAB}working${TAB}ts${TAB}TS${TAB}started_ts${TAB}TS" \
"$(bodies | sed -E 's/[0-9]{10}/TS/g')"

# ---- 2. the end-state leg, once the stem flips (slice 3) ----
run hook claude 1 \
  '{"hook_event_name":"Stop","session_id":"s1","cwd":"/tmp/proj"}'
expect "KDD_LEGS=claude is one authenticated leg on the claude stem" \
"ARGV --app kdeskdash --stem KDASH_CLAUDE_REDIS --best-effort batch" \
"$(argvs)"

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

[ "$fails" -eq 0 ] || { printf '\n%d assertion(s) failed\n' "$fails"; exit 1; }
printf '\nall batch-shape assertions passed\n'
