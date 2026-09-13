#!/bin/sh
# Static contract checks on deploy/kdeskdash.service and the key names the panel
# reads. In CI, because the thing that goes wrong here goes wrong silently: a
# unit that reads a private copy of a fleet password keeps rendering live data
# from a credential nobody maintains, and keeps doing it until the copy drifts.
# That is what happened on 2026-09-05.
#
# Run by `just check`. Exit 1 with a reason, never a bare status.
set -eu

repo=$(cd "$(dirname "$0")/.." && pwd)
unit="$repo/deploy/kdeskdash.service"
fail=0

say() {
    echo "unit-lint: $1" >&2
    fail=1
}

# 1. The unit must read the fleet's per-host secrets file. This is the whole
#    point of sprint 037: one copy of each password per host, rendered by
#    k-homelab, read by every tool that needs it.
grep -q '^EnvironmentFile=-\?/etc/khomelab/secrets.env$' "$unit" ||
    say "the unit does not read /etc/khomelab/secrets.env — the fleet password
          file is where REDISCLI_AUTH and KVSCF_REDISCLI_AUTH come from"

# 2. SupplementaryGroups=khomelab is a REGRESSION, not a hardening. systemd
#    reads EnvironmentFile= as PID 1 before dropping privileges, so the group
#    grants nothing — and a group that does not exist on a host makes the unit
#    fail to start, converting the optional `-` read into a hard dependency on
#    k-homelab having converged. Measured on systemd 259 (korg handoff 2493).
#    This unit runs as root, so it is doubly pointless here.
! grep -q '^SupplementaryGroups=.*khomelab' "$unit" ||
    say "SupplementaryGroups=khomelab is set — it buys nothing (EnvironmentFile=
          is read as PID 1) and makes the unit fail to start on a host where the
          group is absent"

# 3. The retired private key names must not come back as anything that ACTS.
#    Each was a second copy of a secret that now has exactly one name
#    fleet-wide; reintroducing one is how a panel quietly goes back to its own
#    copy. Only two forms do anything — a getenv() in the code, and an
#    assignment in an env file — so only those two fail. Naming a retired
#    variable in prose is how the history stays readable, and sprint 031's
#    comment in config.c is worth more than a clean grep.
for retired in \
    KDESKDASH_TELEMETRY_REDISCLI_AUTH \
    KDESKDASH_CLAUDE_REDISCLI_AUTH \
    KDESKDASH_KVSCF_REDISCLI_AUTH; do
    hits=$(cd "$repo" && git grep -l -E "getenv\(\"$retired\"\)" -- src tests 2>/dev/null || true)
    [ -z "$hits" ] ||
        say "retired key name $retired is read again in: $hits
          (the fleet names are REDISCLI_AUTH and KVSCF_REDISCLI_AUTH)"

    hits=$(cd "$repo" && git grep -l -E "^$retired=" -- 'deploy/**.env' 'deploy/*.env*' 2>/dev/null || true)
    [ -z "$hits" ] ||
        say "retired key name $retired is assigned in: $hits
          (that value now comes from /etc/khomelab/secrets.env)"
done

# 4. The control Redis must NOT read bare REDISCLI_AUTH. That name is the
#    fleet's central rpi53 password and is always set once the unit reads the
#    file above; the control handle talks to this board's own passwordless
#    instance, and a Redis with no password configured answers AUTH with an
#    ERROR. test_config covers the behaviour; this covers the name, because the
#    name is what a well-meaning edit changes.
grep -q 'getenv("KDESKDASH_CONTROL_REDISCLI_AUTH")' "$repo/src/config.c" ||
    say "src/config.c no longer reads KDESKDASH_CONTROL_REDISCLI_AUTH for the
          control handle — if it fell back to bare REDISCLI_AUTH it will AUTH
          against this board's passwordless local instance and never connect"

if [ "$fail" -ne 0 ]; then
    echo "unit-lint: FAILED" >&2
    exit 1
fi
echo "unit-lint: ok — unit reads the per-host file, no group regression, no retired key names"
