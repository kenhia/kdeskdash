#!/usr/bin/env bash
# codex-batch-shape.sh — what codex-pub.sh hands to kdash-pub, asserted exactly.
#
# Same harness as batch-shape.sh and ghcp-batch-shape.sh: a stub kdash-pub
# records argv and stdin, no network. A second stub stands in for `codex`, and
# it reproduces the one behaviour of the real app-server that shaped the
# script: it EXITS ON STDIN EOF WITHOUT ANSWERING. A publisher that pipes its
# requests in and closes stdin passes against a friendlier stub and publishes
# nothing in production.
#
# The fixtures under fixtures/codex/ are the real exchange captured from
# codex-cli 0.171.0 on kai in sprint 049 (`accountId` and `installationId`
# zeroed): app-server-prolite.jsonl and rollout-prolite.jsonl verbatim, the
# logged-out error as the real scratch-CODEX_HOME answer, and
# app-server-plus.jsonl derived from the prolite capture with both windows
# filled — and its by-id copy deliberately disagreeing, so a parser that reads
# the wrong snapshot fails here.

set -u
here=$(cd "$(dirname "$0")" && pwd)
script="$here/../codex-pub.sh"
fix="$here/fixtures/codex"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

export HOSTNAME=testhost
export KDD_STATE_DIR="$work/state"
export KDD_CAPTURE="$work/cap"

cat > "$work/kdash-pub" <<'STUB'
#!/usr/bin/env bash
printf 'ARGV %s\n' "$*" >> "$KDD_CAPTURE"
case " $* " in
  *' hget '*)  printf '%s' "${KDD_STORED:-}" ;;
  *' batch '*) cat >> "$KDD_CAPTURE" ;;
esac
STUB

# Answers only once request 2 has arrived, and only while stdin is open.
cat > "$work/codex" <<'STUB'
#!/usr/bin/env bash
[ "$1" = app-server ] || exit 2
[ -n "${KDD_FIXTURE:-}" ] || exit 1
while IFS= read -r line; do
  case "$line" in *'"id":2,'*) cat "$KDD_FIXTURE" ;; esac
done
STUB
chmod +x "$work/kdash-pub" "$work/codex"
export KDD_PUB_BIN="$work/kdash-pub"
export KDD_CODEX_BIN="$work/codex"

fails=0
ok()   { printf 'ok   %s\n' "$1"; }
fail() { printf 'FAIL %s\n     %s\n' "$1" "$2"; fails=$((fails + 1)); }

# run <app-server fixture|""> <codex home> <stored updated_at>
T0=0 ; T1=0
run() {
  rm -f "$KDD_CAPTURE" ; rm -rf "$KDD_STATE_DIR"
  export KDD_FIXTURE="$1" CODEX_HOME="$2" KDD_STORED="$3"
  T0=$(date +%s)
  timeout 30 "$script" poll >/dev/null 2>&1
  T1=$(date +%s)
}

# The batch line, tabs shown as spaces, updated_at replaced by NOW when it
# falls inside the run (an app-server sample is stamped at observation time).
batch() {
  local line u
  line=$(grep -v '^ARGV ' "$KDD_CAPTURE" 2>/dev/null | tr '\t' ' ')
  u=$(printf '%s' "$line" | sed -n 's/.* updated_at \([0-9]*\) .*/\1/p')
  if [ -n "$u" ] && [ "$u" -ge "$T0" ] && [ "$u" -le "$T1" ]; then
    line=${line/updated_at $u /updated_at NOW }
  fi
  printf '%s' "$line"
}

expect() {
  if [ "$2" = "$3" ]; then ok "$1"; else fail "$1" "want: $2"$'\n     got:  '"$3"; fi
}

empty_home="$work/home-empty" ; mkdir -p "$empty_home"
roll_home="$work/home-rollout"
mkdir -p "$roll_home/sessions/2026/11/24"
cp "$fix/rollout-prolite.jsonl" "$roll_home/sessions/2026/11/24/rollout-2026-11-24T17-46-06-x.jsonl"
# An older rollout must not be the one read.
printf '%s\n' '{"timestamp":"2026-11-23T00:00:00.000Z","type":"event_msg","payload":{"type":"token_count","rate_limits":{"primary":{"used_percent":77.0,"window_minutes":10080,"resets_at":1}}}}' \
  > "$roll_home/sessions/2026/11/24/rollout-2026-11-24T01-00-00-y.jsonl"
ROLL_T=$(date -u -d 2026-11-25T01:53:27Z +%s)

# 1. Pro: one weekly window in the PRIMARY slot -> seven-day, five_hour_none=1.
run "$fix/app-server-prolite.jsonl" "$empty_home" ""
expect "prolite: primary 10080 publishes as seven-day with five_hour_none=1" \
  "hset codex:limits five_hour_none 1 five_hour_pct 0 five_hour_resets_at 0 seven_day_pct 0 seven_day_resets_at 1796175971 updated_at NOW expected_refresh_s 300 host testhost source app-server plan_type prolite" \
  "$(batch)"
expect "prolite: central stem, guard read before the write" \
  "ARGV --app kdeskdash --stem KDASH_CENTRAL_REDIS --best-effort hget codex:limits updated_at
ARGV --app kdeskdash --stem KDASH_CENTRAL_REDIS --best-effort batch" \
  "$(grep '^ARGV ' "$KDD_CAPTURE")"

# 2. Plus: both windows, mapped by length; the by-id copy (99) is not read.
run "$fix/app-server-plus.jsonl" "$empty_home" ""
expect "plus: 300 -> five_hour, 10080 -> seven_day, rateLimits not the by-id copy" \
  "hset codex:limits five_hour_none 0 five_hour_pct 42.5 five_hour_resets_at 1795579200 seven_day_pct 11.5 seven_day_resets_at 1796104200 updated_at NOW expected_refresh_s 300 host testhost source app-server plan_type plus" \
  "$(batch)"

# 3. Logged out, no rollouts: nothing at all reaches Redis — not a zero.
run "$fix/app-server-logged-out.jsonl" "$empty_home" ""
expect "logged out, no rollout: publishes nothing" "" "$(cat "$KDD_CAPTURE" 2>/dev/null)"
expect "logged out: breadcrumb names the reason" \
  "codex account authentication required to read rate limits" \
  "$(sed 's/ as of .*//' "$KDD_STATE_DIR/app-server-error" 2>/dev/null)"

# 4. Logged out with a rollout: the rollout, stamped with ITS OWN time.
run "$fix/app-server-logged-out.jsonl" "$roll_home" ""
expect "fallback: newest rollout, observation time from the event" \
  "hset codex:limits five_hour_none 1 five_hour_pct 0 five_hour_resets_at 0 seven_day_pct 0.0 seven_day_resets_at 1796175971 updated_at $ROLL_T expected_refresh_s 300 host testhost source rollout plan_type prolite" \
  "$(batch)"

# 5. Guard: the same observation already stored -> no republish (CD-33).
run "" "$roll_home" "$ROLL_T"
expect "guard: an equal stored stamp is not republished" "" "$(batch)"

# 6. Guard: a newer stored stamp wins over an old rollout.
run "" "$roll_home" "$((ROLL_T + 600))"
expect "guard: a newer stored stamp is not overwritten" "" "$(batch)"

# 7. Guard: an older stored stamp is replaced.
run "" "$roll_home" "$((ROLL_T - 600))"
case "$(batch)" in *"updated_at $ROLL_T "*) ok "guard: an older stored stamp is replaced" ;;
  *) fail "guard: an older stored stamp is replaced" "got: $(batch)" ;; esac

# 8. A sample with no 10080-minute window is incomplete: nothing published.
sed 's/"windowDurationMins":10080/"windowDurationMins":60/g' "$fix/app-server-prolite.jsonl" > "$work/no-week.jsonl"
run "$work/no-week.jsonl" "$empty_home" ""
expect "incomplete sample (no weekly window): publishes nothing" "" "$(batch)"

# 9. No codex binary and no rollout: nothing, and a breadcrumb.
KDD_CODEX_BIN="$work/absent" PATH=/usr/bin:/bin HOME="$work/nohome" run "" "$empty_home" ""
expect "no codex binary: publishes nothing" "" "$(batch)"

[ "$fails" -eq 0 ] && { echo "codex-batch-shape: all passed"; exit 0; }
echo "codex-batch-shape: $fails failed"; exit 1
