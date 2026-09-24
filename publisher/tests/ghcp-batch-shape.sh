#!/usr/bin/env bash
# ghcp-batch-shape.sh — what ghcp-pub.sh hands to kdash-pub, asserted exactly.
#
# Same harness as batch-shape.sh (a stub kdash-pub recording argv and stdin, no
# network), and the same reason: one off-contract line makes a whole batch
# publish nothing, so the format is worth pinning.
#
# What is different here is the INPUT. Every payload below was captured from a
# live Copilot CLI 1.0.85 session on kai during sprint 038 and is stored
# verbatim under fixtures/ghcp/ — so this is a test against what Copilot
# actually sends, not against a reconstruction of it. The event names are read
# out of the SHIPPED ghcp-hooks.json rather than restated here, because a
# mistyped Copilot event name fires nothing and warns nothing: a template that
# drifted from the measured six would be invisible everywhere else.

set -u
here=$(cd "$(dirname "$0")" && pwd)
script="$here/../ghcp-pub.sh"
hooks="$here/../ghcp-hooks.json"
fix="$here/fixtures/ghcp"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

export KDD_STATE_DIR="$work/state"
export HOSTNAME=testhost
mkdir -p "$KDD_STATE_DIR"

cat > "$work/kdash-pub" <<'STUB'
#!/usr/bin/env bash
out=$(mktemp "$KDD_CAPTURE_DIR/leg.XXXXXX")
printf 'ARGV %s\n' "$*" > "$out"
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
  local event="$1" want_legs="$2" stdin="$3" i
  export KDD_CAPTURE_DIR="$work/cap"
  rm -rf "$KDD_CAPTURE_DIR"; mkdir -p "$KDD_CAPTURE_DIR"
  printf '%s' "$stdin" | "$script" "$event" >/dev/null 2>&1
  for i in $(seq 1 100); do
    [ "$(find "$KDD_CAPTURE_DIR" -type f | wc -l)" -ge "$want_legs" ] && break
    sleep 0.05
  done
}

runf() { run "$1" "$2" "$(cat "$fix/$3")"; }

bodies() { cat "$KDD_CAPTURE_DIR"/* 2>/dev/null | grep -v '^ARGV ' | sort; }
argvs()  { grep -h '^ARGV ' "$KDD_CAPTURE_DIR"/* 2>/dev/null | sort; }

expect() {
  local name="$1" want="$2" got="$3"
  if [ "$got" = "$want" ]; then ok "$name"
  else fail "$name" "want: $want
     got:  $got"; fi
}

TAB=$'\t'
SID=22bc4947-9acf-4320-b1b3-e41fbb40789b
KEY="ghcp:session:testhost:$SID"

# ---- 1. the template declares exactly the events the publisher acts on ------
# A mistyped Copilot event name fires nothing and warns nothing, so the
# template is checked against the measured set rather than trusted. The six
# measured on 1.0.85 are sessionStart, sessionEnd, userPromptSubmitted,
# preToolUse, postToolUse, errorOccurred; errorOccurred is deliberately NOT
# wired (no status this feed can honestly publish from it), so the template
# must declare the other five and nothing else.
declared=$(grep -oE '^    "[a-zA-Z]+": \[' "$hooks" | sed -E 's/^ *"//; s/": \[//' | sort | tr '\n' ' ')
expect "the hook template declares exactly the five wired events" \
"postToolUse preToolUse sessionEnd sessionStart userPromptSubmitted " \
"$declared"

# Every command in the template invokes the publisher with its own event name
# as argv[1] — Copilot puts the event name nowhere in the payload, so a
# copy-paste that left the wrong name would publish one event under another.
bad=""
while read -r ev; do
  [ -n "$ev" ] || continue
  n=$(grep -c "ghcp-pub.sh' *$ev\"\|ghcp-pub\.sh $ev\"" "$hooks")
  [ "$n" -eq 2 ] || bad="$bad $ev($n)"
done <<< "$(printf '%s\n' $declared)"
expect "every template command passes its own event name as argv[1]" "" "$bad"

# ---- 2. sessionStart: the whole record, and MILLISECONDS CONVERTED ----------
# The sharpest hazard on this feed. Copilot's `timestamp` is milliseconds
# (1789622590936); published unconverted it is a stamp ~1000x further in the
# future than now, every reader treats a negative age as clock skew and
# therefore fresh, and the row reads as permanently live with nothing
# reporting an error. 1789622590936 / 1000 = 1789622590, asserted literally
# rather than through a TS placeholder, because the placeholder is exactly
# what would hide a unit slip.
runf sessionStart 1 sessionStart.json
expect "the shipped default is one authenticated leg on the claude stem" \
"ARGV --app kdeskdash --stem KDASH_CLAUDE_REDIS --best-effort batch" \
"$(argvs)"
expect "sessionStart publishes the full record with ts converted to SECONDS" \
"expire${TAB}${KEY}${TAB}7200
hset${TAB}${KEY}${TAB}host${TAB}testhost${TAB}project${TAB}ghcp-probe-work${TAB}cwd${TAB}/tmp/ghcp-probe-work${TAB}status${TAB}working${TAB}ts${TAB}1789622590${TAB}started_ts${TAB}1789622590" \
"$(bodies)"

# ---- 3. no prompt text ever reaches Redis (claude-pub.sh's R20) ------------
# sessionStart carries `initialPrompt` and userPromptSubmitted carries
# `prompt`. Both fixtures contain the probe's prompt text; neither event may
# put a word of it on the wire.
leak=0
for f in sessionStart.json userPromptSubmitted.json; do
  ev="${f%.json}"
  runf "$ev" 1 "$f"
  bodies | grep -qi 'sample.txt\|Run the shell command\|nothing else' && leak=1
done
expect "no prompt text from any event reaches the batch" "0" "$leak"

# ---- 4. userPromptSubmitted does NOT write started_ts ----------------------
# It fires ~5 ms BEFORE sessionStart in -p mode (measured, both 1.0.83 and
# 1.0.85), so it is usually the first write. started_ts belongs to the
# sessionStart payload's own stamp; claiming it here would pin session start
# to whichever event happened to arrive first.
runf userPromptSubmitted 1 userPromptSubmitted.json
expect "userPromptSubmitted publishes status+ts and never started_ts" \
"expire${TAB}${KEY}${TAB}7200
hset${TAB}${KEY}${TAB}host${TAB}testhost${TAB}project${TAB}ghcp-probe-work${TAB}cwd${TAB}/tmp/ghcp-probe-work${TAB}status${TAB}working${TAB}ts${TAB}1789622590" \
"$(bodies)"

# ---- 5. the out-of-order pair converges on the payload's own started_ts ----
# Replay the real order — userPromptSubmitted first, then sessionStart — and
# assert the record ends up with started_ts from the sessionStart payload.
# hset merges, so the later event adds the field rather than replacing a hash.
runf userPromptSubmitted 1 userPromptSubmitted.json
first="$(bodies)"
runf sessionStart 1 sessionStart.json
second="$(bodies)"
if printf '%s' "$first" | grep -q 'started_ts'; then
  fail "out-of-order pair: started_ts comes only from sessionStart" \
       "userPromptSubmitted wrote started_ts"
elif printf '%s' "$second" | grep -q "started_ts${TAB}1789622590"; then
  ok "out-of-order pair: started_ts comes only from sessionStart"
else
  fail "out-of-order pair: started_ts comes only from sessionStart" "$second"
fi

# ---- 6. tool events are a ts-only keepalive, and throttled -----------------
runf preToolUse 1 preToolUse.json
expect "preToolUse is a ts-only keepalive that re-arms the TTL" \
"expire${TAB}${KEY}${TAB}7200
hset${TAB}${KEY}${TAB}ts${TAB}1789622603" \
"$(bodies)"

# The second one inside the 2-minute window must publish nothing at all: this
# is the hottest path in the script, once per tool call in every session.
runf postToolUse 0 postToolUse.json
sleep 0.2
expect "a second tool event inside the throttle window publishes nothing" "0" \
"$(find "$KDD_CAPTURE_DIR" -type f | wc -l)"

# ---- 7. a nested key in toolArgs cannot hijack a top-level field ----------
# toolArgs holds a tool's own arguments, so its KEYS are outside our control.
# A tool taking an argument called `timestamp` or `cwd` is ordinary, and the
# nested key appears LATER in the document than the real one. The helpers take
# the FIRST match and Copilot emits top-level fields first, so the nested copy
# must lose.
#
# This has to be a nested object key and not a decoy inside a string: a JSON
# string cannot contain a bare `"timestamp":0` (the quotes would have to be
# escaped), so a string-borne decoy tests nothing. Verified by mutation — with
# the helpers changed to take the LAST match, this assertion fails and a
# string-borne one still passes.
#
# The cost of getting it wrong is silent: ts 0 fails the schema's
# exclusiveMinimum, and the batch is best-effort, so the row simply stops
# updating with nothing reporting an error.
rm -f "$KDD_STATE_DIR/$SID.hb"
run preToolUse 1 \
  "{\"sessionId\":\"$SID\",\"timestamp\":1789622603053,\"cwd\":\"/tmp/ghcp-probe-work\",\"toolName\":\"bash\",\"toolArgs\":{\"timestamp\":0,\"cwd\":\"/tmp/hijacked\",\"command\":\"date\"}}"
expect "a nested timestamp in toolArgs does not become the published ts" \
"expire${TAB}${KEY}${TAB}7200
hset${TAB}${KEY}${TAB}ts${TAB}1789622603" \
"$(bodies)"

# The same hazard on a string field: sessionStart's cwd decides `project`, and
# a nested `cwd` must not redirect it.
run sessionStart 1 \
  "{\"sessionId\":\"s9\",\"timestamp\":1789622590936,\"cwd\":\"/tmp/real\",\"extra\":{\"cwd\":\"/tmp/hijacked\"}}"
expect "a nested cwd does not become the published cwd or project" \
"expire${TAB}ghcp:session:testhost:s9${TAB}7200
hset${TAB}ghcp:session:testhost:s9${TAB}host${TAB}testhost${TAB}project${TAB}real${TAB}cwd${TAB}/tmp/real${TAB}status${TAB}working${TAB}ts${TAB}1789622590${TAB}started_ts${TAB}1789622590" \
"$(bodies)"

# ---- 8. a refused tool still keeps the row alive --------------------------
# Measured: a denied tool fires preToolUse and NO postToolUse. Its toolArgs is
# a bare STRING rather than an object, which a parser assuming an object would
# trip over. The session is still very much alive, so the keepalive must land.
rm -f "$KDD_STATE_DIR/cf35e2f8-e314-4321-b700-cbb601b131ab.hb"
runf preToolUse 1 preToolUse-denied.json
expect "a refused tool (string toolArgs, no postToolUse) still keepalives" \
"expire${TAB}ghcp:session:testhost:cf35e2f8-e314-4321-b700-cbb601b131ab${TAB}7200
hset${TAB}ghcp:session:testhost:cf35e2f8-e314-4321-b700-cbb601b131ab${TAB}ts${TAB}1789622652" \
"$(bodies)"

# ---- 9. sessionEnd is a bare DEL — and no ghcp:recent ---------------------
# The registry keeps `ghcp:recent` optional and UNSCHEMA'D. A publisher
# inventing it would be minting a contract this script does not own, so the
# DEL is the whole of the event. Both reasons observed in the wild are
# checked: `complete` from -p, `user_exit` from an interactive quit.
runf sessionEnd 1 sessionEnd.json
expect "sessionEnd DELs the key and writes nothing else" \
"del${TAB}${KEY}" \
"$(bodies)"
run sessionEnd 1 "$(cat "$fix/sessionEnd-user_exit.json")"
expect "an interactive quit (reason user_exit) DELs the key just the same" \
"del${TAB}ghcp:session:testhost:a70fee52-9105-48e9-a7b8-150ea748d966" \
"$(bodies)"

# ---- 10. an event this feed cannot speak for publishes nothing ------------
# errorOccurred is real and measured, and is deliberately not wired: there is
# no status it could honestly produce. Same for anything Copilot adds later.
run errorOccurred 0 "{\"sessionId\":\"$SID\",\"timestamp\":1789622605168,\"cwd\":\"/tmp/p\"}"
sleep 0.2
expect "an unwired event publishes nothing rather than guessing" "0" \
"$(find "$KDD_CAPTURE_DIR" -type f | wc -l)"

# No argument at all: the event name is argv[1] here, so a hook declaration
# that forgot it must be inert rather than publishing an arbitrary branch.
export KDD_CAPTURE_DIR="$work/cap"
rm -rf "$KDD_CAPTURE_DIR"; mkdir -p "$KDD_CAPTURE_DIR"
printf '{"sessionId":"x","timestamp":1789622605168,"cwd":"/tmp/p"}' \
  | "$script" >/dev/null 2>&1
sleep 0.2
expect "no event argument publishes nothing" "0" \
"$(find "$KDD_CAPTURE_DIR" -type f | wc -l)"

# ---- 11. a value carrying the delimiter cannot split a line ---------------
# `cwd` reaches the batch straight off the wire. A LITERAL tab in the payload
# would otherwise shift every following field by one and silently rewrite the
# record.
run sessionStart 1 \
  "$(printf '{"sessionId":"s2","timestamp":1789622590936,"cwd":"/tmp/a\tb"}')"
expect "a literal tab inside a field is stripped, not emitted" \
"expire${TAB}ghcp:session:testhost:s2${TAB}7200
hset${TAB}ghcp:session:testhost:s2${TAB}host${TAB}testhost${TAB}project${TAB}ab${TAB}cwd${TAB}/tmp/ab${TAB}status${TAB}working${TAB}ts${TAB}1789622590${TAB}started_ts${TAB}1789622590" \
"$(bodies)"

# ---- 12. a missing or absurd timestamp never publishes an off-schema ts ---
# The schema bounds ts at (0, 1e11]. A record outside it is rejected outright,
# and because the batch is best-effort the rejection is silent — so the
# publisher clamps to its own clock rather than emitting one.
run sessionStart 1 '{"sessionId":"s3","cwd":"/tmp/p"}'
got=$(bodies | grep -oE "ts${TAB}[0-9]+" | head -n1 | tr -d "$TAB" | sed 's/^ts//')
if [ -n "$got" ] && [ "$got" -gt 0 ] && [ "$got" -le 100000000000 ]; then
  ok "a missing timestamp falls back to a stamp inside the schema's bound"
else
  fail "a missing timestamp falls back to a stamp inside the schema's bound" "ts=$got"
fi

run sessionStart 1 '{"sessionId":"s4","timestamp":999999999999999,"cwd":"/tmp/p"}'
got=$(bodies | grep -oE "ts${TAB}[0-9]+" | head -n1 | tr -d "$TAB" | sed 's/^ts//')
if [ -n "$got" ] && [ "$got" -gt 0 ] && [ "$got" -le 100000000000 ]; then
  ok "an absurd timestamp is brought inside the schema's bound"
else
  fail "an absurd timestamp is brought inside the schema's bound" "ts=$got"
fi

# ---- 13. lowercase verbs: kdash-pub's parser rejects HSET -----------------
runf sessionStart 1 sessionStart.json
if bodies | grep -qE '^[A-Z]'; then
  fail "every verb is lowercase" "$(bodies | grep -E '^[A-Z]')"
else
  ok "every verb is lowercase"
fi

# ---- 14. a host with no kdash-pub leaves a findable breadcrumb ------------
rm -rf "$work/cap"; mkdir -p "$work/cap"
printf '%s' "$(cat "$fix/sessionStart.json")" \
  | KDD_PUB_BIN="$work/not-installed" "$script" sessionStart >/dev/null 2>&1
if [ -s "$KDD_STATE_DIR/no-kdash-pub" ]; then
  ok "a missing kdash-pub leaves a breadcrumb"
else
  fail "a missing kdash-pub leaves a breadcrumb" "no $KDD_STATE_DIR/no-kdash-pub"
fi

# ---- 15. no hand-rolled sockets ------------------------------------------
# Routing through kdash-pub is what makes khlenv, AUTH and the key grammar
# non-optional. A /dev/tcp anywhere is a path around all three.
if grep -v '^[[:space:]]*#' "$script" | grep -q 'dev/tcp'; then
  fail "no hand-rolled sockets" "$(grep -n 'dev/tcp' "$script" | grep -v ':[[:space:]]*#')"
else
  ok "no hand-rolled sockets"
fi

# ---- 16. the darwin template is the Linux one, re-homed --------------------
# ghcp-hooks.darwin.json exists because a Copilot hook command is not
# $HOME-expanded and kimac's $HOME is /Users/ken (korg WI 3170). k-homelab's
# copilot-hooks installs it byte-identical on a Darwin host, so it must wire
# the same five events, each passing its own name, with the same timeouts —
# differing ONLY in the publisher path (and in carrying no Windows variant,
# which a Mac never reads). Derived from the Linux file rather than restated,
# so a sixth event added to one template and not the other fails here.
darwin="$here/../ghcp-hooks.darwin.json"
if [ -f "$darwin" ]; then
  want_darwin=$(grep -v '"powershell":' "$hooks" | sed 's#"/home/ken/#"/Users/ken/#')
  expect "the darwin template is the Linux one with only the path re-homed" \
    "$want_darwin" "$(cat "$darwin")"
  expect "every darwin bash command runs /Users/ken's publisher" "" \
    "$(grep '"bash":' "$darwin" | grep -v '"bash": "/Users/ken/.copilot/kdeskdash-pub/ghcp-pub.sh [a-zA-Z]*",$')"
else
  fail "the darwin template ships" "no $darwin"
fi

[ "$fails" -eq 0 ] || { printf '\n%d assertion(s) failed\n' "$fails"; exit 1; }
printf '\nall ghcp batch-shape assertions passed\n'
