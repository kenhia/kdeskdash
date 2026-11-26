#!/usr/bin/env bash
# Deploy / service management for kdeskdash on the Pi.
#
# Usage:
#   scripts/deploy.sh deploy          <target> [version]
#   scripts/deploy.sh install-service <target> <env-file> [version]
#   scripts/deploy.sh push-dev        <target> <binary> [ttf]
#   scripts/deploy.sh fetch           [version]
#   scripts/deploy.sh versions        <target>...
#   scripts/deploy.sh publisher-windows <target> [version]   (the claude-feed publisher, cleo)
#
# Since sprint 024 a deploy installs a *published artifact*, not this build
# tree: `just publish` puts artifacts/kdeskdash/<version>/ in the homelab
# package store, and this script fetches that version, checksum-verifies it,
# and pushes it to a board. Naming an older version is the rollback — the
# store's history is the only reason one exists.
#
# The fetch happens HERE, on the dev box, not on the board. The Pis are
# deliberately unmanaged: no tailnet, no store credentials, nothing to keep
# current on a device whose recovery story is a keyboard and a monitor. The
# artifact reaches them the way everything else does, over the ssh that was
# already the deploy path.
#
# The target is any ssh destination, so one build serves every dashboard Pi.
# Keeping the remote shell pipelines here (instead of inline in the justfile)
# avoids fragile nested-quote escaping through just -> sh -> ssh.
#
# Env (from .env on the dev box, no defaults on purpose):
#   KDESKDASH_STORE_URL    package store base, e.g. https://host:4880
#   KDESKDASH_STORE_HOST   host running kpkg — only `versions` needs it
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
cache="$repo/.deploy-cache"

# Where a fetched version lands on the board.
BIN=/usr/local/bin/kdeskdash
FONT_DIR=/usr/local/share/kdeskdash
FONT=SymbolsNerdFont-Regular.ttf

die() {
    echo "deploy.sh: $1" >&2
    exit 1
}

# die, but drop a half-fetched cache directory first. Not a trap: fetch_artifact
# runs inside a command substitution, so an EXIT trap would fire on every normal
# return of that subshell too.
fetch_failed() {
    rm -rf "$1"
    die "$2"
}

# Resolve `latest` (or take the version given), fetch the artifact into
# .deploy-cache/<version>/, verify every file against SHA256SUMS, and echo the
# directory. A cached copy that still verifies is reused — the store is
# immutable, so a version that checks out cannot have changed underneath us.
#
# The second argument names the artifact (default: the panel's). The publisher
# bundle caches under .deploy-cache/kdeskdash-publisher/<version>/ so the
# panel's own cache listing in `versions` stays a list of panel versions.
fetch_artifact() {
    local want=${1:-} artifact=${2:-kdeskdash}
    : "${KDESKDASH_STORE_URL:?set KDESKDASH_STORE_URL in .env (e.g. https://kubsdb.encke-wahoo.ts.net:4880)}"
    local base="$KDESKDASH_STORE_URL/artifacts/$artifact"

    local v="$want"
    if [ -z "$v" ]; then
        # No `latest` is not the same as nothing published: publishing off main
        # deliberately leaves the pointer alone, so a branch-only artifact tree
        # has versions and no newest. Name one.
        v=$(curl -fsS "$base/latest") ||
            die "no \`latest\` pointer at $base — nothing has been published from main yet. Name a version (just versions)."
        v=$(printf '%s' "$v" | tr -d '[:space:]')
    fi

    local dir="$cache/$v"
    [ "$artifact" = kdeskdash ] || dir="$cache/$artifact/$v"
    if [ -f "$dir/SHA256SUMS" ] && (cd "$dir" && sha256sum -c --quiet SHA256SUMS 2>/dev/null); then
        echo "using cached artifact $v" >&2
        printf '%s\n' "$dir"
        return
    fi

    echo "fetching $artifact $v from the store" >&2
    rm -rf "$dir"
    mkdir -p "$dir"
    # Every failure from here on takes the directory with it. Leaving a partial
    # one behind is harmless to a later deploy (it will not verify, so it gets
    # re-fetched) but `versions` lists the cache by directory name, and a cache
    # that names a version it does not have is the kind of small lie the whole
    # store conversion exists to remove.
    curl -fsS -o "$dir/SHA256SUMS" "$base/$v/SHA256SUMS" ||
        fetch_failed "$dir" "no artifact for version '$v' in the store (try: just versions)"
    # Drive the download off SHA256SUMS rather than a hardcoded file list, so an
    # older version that shipped a different set of files still fetches whole.
    local sum name
    while read -r sum name; do
        [ -n "${sum:-}" ] || continue
        curl -fsS -o "$dir/$name" "$base/$v/$name" || fetch_failed "$dir" "fetch failed: $name"
    done < "$dir/SHA256SUMS"
    (cd "$dir" && sha256sum -c --quiet SHA256SUMS) ||
        fetch_failed "$dir" "checksum mismatch in $v — refusing to deploy it"

    printf '%s\n' "$dir"
}

# Ask the binary installed on a board what it is. Echoes its answer, or nothing.
#
# The `timeout -k` is load-bearing, not caution. A pre-sprint-024 binary has no
# --version: it ignores the unknown argument and *starts the dashboard*, so the
# probe that exists to detect a failed install is exactly the case that would
# hang on one — and leave a second instance running on the panel. It also will
# not die of SIGTERM while blocked in DRM init, hence the -k SIGKILL.
probe_version() {
    ssh -n "$1" "timeout -k 2 10 $BIN --version </dev/null 2>/dev/null" 2>/dev/null || true
}

# Install a binary (and optionally a font) on a board. Shared by `deploy` and
# `push-dev` so the dev-loop path cannot drift from the real one.
push_binary() {
    local target=$1 binary=$2 ttf=${3:-}
    # Stop the service if installed; fall back to killing a manually-run
    # instance. scp fails with "text file busy" if the binary is still running.
    ssh "$target" 'sudo systemctl stop kdeskdash 2>/dev/null || true; sudo pkill -INT kdeskdash 2>/dev/null || true; sleep 1'
    # Stage in $HOME (scp has no sudo), then install to a root-owned path so the
    # root service does not execute a binary from a user-writable directory.
    scp -q "$binary" "$target:~/kdeskdash.new"
    ssh "$target" "sudo install -m755 ~/kdeskdash.new $BIN && rm -f ~/kdeskdash.new"

    # Icons-mode assets: the runtime font and the writable state dir for the
    # favourites file. Both are optional to the app (the mode shows an
    # unavailable state without the font). The font is 2.4MB and byte-identical
    # across most releases, so compare first and only send it when it differs —
    # but *do* send it when it does, because it is versioned with the binary now.
    if [ -n "$ttf" ]; then
        ssh "$target" "sudo install -d -m755 $FONT_DIR /var/lib/kdeskdash"
        local want have
        want=$(sha256sum "$ttf" | cut -d' ' -f1)
        have=$(ssh "$target" "sha256sum $FONT_DIR/$FONT 2>/dev/null | cut -d' ' -f1" || true)
        if [ "$want" != "$have" ]; then
            scp -q "$ttf" "$target:~/kdeskdash-nf.ttf"
            ssh "$target" "sudo install -D -m644 ~/kdeskdash-nf.ttf $FONT_DIR/$FONT && rm -f ~/kdeskdash-nf.ttf"
            echo "installed Nerd Font to $target:$FONT_DIR/"
        fi
    fi
}

cmd=${1:?usage: deploy.sh <deploy|install-service|push-dev|fetch|versions|publisher-windows> ...}
shift || true

case "$cmd" in
    deploy)
        target=${1:?missing ssh target}
        version=${2:-}
        dir=$(fetch_artifact "$version")
        v=$(basename "$dir")

        push_binary "$target" "$dir/kdeskdash-aarch64-linux" "$dir/$FONT"

        # Prove the push landed. A health check cannot do this — the old
        # process answers just as well, and on a panel that is *especially*
        # convincing because the screen keeps showing something. Asking the
        # installed binary what it is compares the version that was fetched
        # against the version that is now on disk, and the checksum above
        # already proved the fetch was the version it claimed.
        got=$(probe_version "$target")
        if [ "$got" != "kdeskdash $v" ]; then
            die "installed binary reports '${got:-nothing}', expected 'kdeskdash $v' — deploy did NOT take"
        fi

        ssh "$target" 'sudo systemctl start kdeskdash 2>/dev/null || true'
        echo "deployed kdeskdash $v to $target:$BIN"
        ;;

    push-dev)
        # The dev loop, and deliberately not a deploy: kdeskdash cannot run on
        # the dev box at all — no DRM panel, no touch — so the board is the only
        # place a change can be looked at, and requiring a commit + publish per
        # glance would make iterating on a layout absurd. k-homelab
        # docs/deploying.md draws the line at *deploys*, and dev iteration is
        # explicitly on the other side of it.
        #
        # The build it pushes is stamped `-dirty` by scripts/version.sh whenever
        # the tree is, so `just versions` shows a board running a build that is
        # not in the store instead of quietly implying it is.
        target=${1:?missing ssh target}
        binary=${2:?missing binary path}
        ttf=${3:-}
        push_binary "$target" "$binary" "$ttf"
        got=$(probe_version "$target")
        got=${got:-unknown}
        ssh "$target" 'sudo systemctl start kdeskdash 2>/dev/null || true'
        echo "pushed dev build to $target:$BIN ($got)"
        echo "note: not a published version — \`just publish\` + \`just deploy\` is what ships"
        ;;

    install-service)
        target=${1:?missing ssh target}
        env_file=${2:?missing env file}
        version=${3:-}
        dir=$(fetch_artifact "$version")
        v=$(basename "$dir")
        # The unit comes from the artifact, so the unit a build was released
        # with is the unit that build runs under. The host's env file comes from
        # the repo: it is device config on its own clock, and this command never
        # overwrites an installed one anyway.
        service="$dir/kdeskdash.service"
        # A host with no committed deploy/hosts/<host>.env yet still gets a
        # working install: fall back to the artifact's example, which is
        # all-defaults.
        if [ ! -f "$env_file" ]; then
            echo "note: $env_file not found — installing $v's kdeskdash.env.example instead" >&2
            echo "      (commit that file to give this host its own config)" >&2
            env_file="$dir/kdeskdash.env.example"
        fi
        scp -q "$service" "$env_file" "$target:~/"
        svc=$(basename "$service")
        env=$(basename "$env_file")
        # Install the unit + an env file (only if absent, so hand edits survive),
        # reload, enable. Secrets are NOT installed here: /etc/kdeskdash/secrets.env
        # is hand-installed once per device (see deploy/hosts/README.md).
        ssh "$target" "
      set -e
      sudo install -D -m644 ~/$svc /etc/systemd/system/kdeskdash.service
      sudo install -d -m755 /etc/kdeskdash
      if [ ! -f /etc/kdeskdash/kdeskdash.env ]; then
        sudo install -m600 ~/$env /etc/kdeskdash/kdeskdash.env
        echo 'installed /etc/kdeskdash/kdeskdash.env from $env'
      else
        echo 'kept existing /etc/kdeskdash/kdeskdash.env (not overwritten)'
      fi
      rm -f ~/$svc ~/$env
      sudo systemctl daemon-reload
      sudo systemctl enable kdeskdash
    "
        # THE UNIT AND THE BINARY ARE NOT INDEPENDENTLY DEPLOYABLE, since
        # sprint 037. The unit reads /etc/khomelab/secrets.env, which sets
        # REDISCLI_AUTH — and a kdeskdash older than 037 reads that bare name as
        # its CONTROL Redis password. The control instance is passwordless, and
        # Redis answers AUTH-with-no-password-configured with an ERROR, so an
        # old binary under a new unit silently loses remote mode control,
        # last-mode persistence, GoL injection and the screenshot trigger while
        # the panel carries on drawing.
        #
        # A warning, not a refusal: a fresh device legitimately has no binary
        # yet, and this command is how it gets its first unit.
        installed=$(probe_version "$target")
        if [ -n "$installed" ] && [ "$installed" != "kdeskdash $v" ]; then
            echo "deploy.sh: WARNING — this unit is from $v but $target is running '$installed'." >&2
            echo "           Since 0.27.x the unit reads /etc/khomelab/secrets.env, and a binary" >&2
            echo "           older than that reads REDISCLI_AUTH as its CONTROL password — which" >&2
            echo "           breaks remote control silently. Run: just deploy ${target#*@} $v" >&2
        fi
        echo "installed kdeskdash service on $target (unit from $v)"
        ;;

    fetch)
        fetch_artifact "${1:-}"
        ;;

    publisher-windows)
        # Install a published claude-feed publisher on a Windows host (cleo):
        # the one feed host k-homelab does not manage, which is how it came to
        # be found three releases behind (korg WI 4651). Ken's ruling there:
        # this repo owns keeping it current, declared in .sprint-deploy after
        # publish-publisher, so every release reaches it in the sprint that
        # cut it.
        #
        # Only claude-pub.sh and VERSION: they are what cleo runs from the
        # bundle. poll-hidden.vbs and the scheduled task are a one-time
        # install (publisher/README.md) and are not in the bundle at all.
        #
        # The remote shell is PowerShell, which parses the whole ssh command
        # string, so no script text crosses that boundary as text: each one is
        # sent as -EncodedCommand (UTF-16LE, base64), whose alphabet no shell
        # on either side has an opinion about. Files go by scp to <name>.new,
        # are hash-checked there BEFORE the swap, and the installed files are
        # hashed again AFTER it — a write that reports success and did not land
        # is the failure mode this exists to catch.
        target=${1:?missing ssh target}
        version=${2:-}
        dir=$(fetch_artifact "$version" kdeskdash-publisher)
        v=$(basename "$dir")
        files=(claude-pub.sh VERSION)
        rdir='.claude/kdeskdash-pub'                   # relative to the ssh home
        ssh_opts=(-o BatchMode=yes -o ConnectTimeout=15)

        # pwsh <script>: run a PowerShell script on the target, encoded.
        pwsh_run() {
            local enc
            enc=$(printf '%s' "$1" | iconv -f UTF-8 -t UTF-16LE | base64 -w0)
            ssh -n "${ssh_opts[@]}" "$target" "pwsh -NoProfile -NonInteractive -EncodedCommand $enc"
        }
        # Prints "<sha256> <name>" per file, "- <name>" for an absent one.
        # ASCII only: this text reaches Windows PowerShell's parser.
        hash_ps="\$ErrorActionPreference = 'Stop'
\$d = Join-Path \$env:USERPROFILE '.claude\\kdeskdash-pub'
if (-not (Test-Path -LiteralPath \$d -PathType Container)) { Write-Output 'NO-DIR'; exit 3 }
foreach (\$n in @($(printf "'%s'," "${files[@]}" | sed 's/,$//'))) {
  \$p = Join-Path \$d \$n
  if (Test-Path -LiteralPath \$p) { Write-Output ((Get-FileHash -Algorithm SHA256 -LiteralPath \$p).Hash.ToLower() + ' ' + \$n) }
  else { Write-Output ('- ' + \$n) }
}"
        want=$(cd "$dir" && sha256sum "${files[@]}" | awk '{print $1" "$2}')

        rc=0
        have=$(pwsh_run "$hash_ps" | tr -d '\r') || rc=$?
        case $rc in
            0) ;;
            3) die "$target has no ~/$rdir — the first install is by hand (publisher/README.md, Windows); this only keeps it current" ;;
            *) die "could not read $target's installed publisher (ssh/pwsh exit $rc) — is it awake? Nothing was changed." ;;
        esac
        if [ "$have" = "$want" ]; then
            echo "$target already runs kdeskdash-publisher $v (hashes match) — nothing to install"
            exit 0
        fi

        # Stage beside the target, so the swap is a rename in one directory.
        for f in "${files[@]}"; do
            scp -q "${ssh_opts[@]}" "$dir/$f" "$target:$rdir/$f.new"
        done

        # Verify every staged file, then swap them in. A staged file that does
        # not match is removed and nothing is swapped. The swap retries: a hook
        # running the old script holds it open, and Windows refuses to replace
        # an open file for the moment it takes the hook to finish.
        swap_ps="\$ErrorActionPreference = 'Stop'
\$d = Join-Path \$env:USERPROFILE '.claude\\kdeskdash-pub'
\$want = @{ $(cd "$dir" && sha256sum "${files[@]}" | awk '{printf "\x27%s\x27 = \x27%s\x27; ", $2, $1}') }
foreach (\$n in \$want.Keys) {
  \$h = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path \$d (\$n + '.new'))).Hash.ToLower()
  if (\$h -ne \$want[\$n]) {
    foreach (\$m in \$want.Keys) { Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath (Join-Path \$d (\$m + '.new')) }
    Write-Output ('staged ' + \$n + ' is ' + \$h + ', expected ' + \$want[\$n]); exit 4
  }
}
foreach (\$n in \$want.Keys) {
  for (\$i = 1; ; \$i++) {
    try { Move-Item -Force -LiteralPath (Join-Path \$d (\$n + '.new')) -Destination (Join-Path \$d \$n); break }
    catch { if (\$i -ge 10) { Write-Output ('could not replace ' + \$n + ': ' + \$_); exit 5 }; Start-Sleep -Milliseconds 500 }
  }
}"
        rc=0
        out=$(pwsh_run "$swap_ps" | tr -d '\r') || rc=$?
        [ "$rc" = 0 ] || die "install on $target failed (exit $rc): $out"

        # Prove it landed, from the target's own reading of its files.
        have=$(pwsh_run "$hash_ps" | tr -d '\r') || die "could not re-read $target after the install"
        if [ "$have" != "$want" ]; then
            printf 'expected:\n%s\nfound:\n%s\n' "$want" "$have" >&2
            die "kdeskdash-publisher $v did NOT land on $target"
        fi
        echo "deployed kdeskdash-publisher $v to $target:~/$rdir (hashes verified on $target)"
        ;;

    versions)
        : "${KDESKDASH_STORE_HOST:?set KDESKDASH_STORE_HOST in .env (the host running kpkg, e.g. kubsdb)}"
        echo "store:"
        ssh -n "$KDESKDASH_STORE_HOST" 'kpkg list' | sed -n 's|^artifacts/kdeskdash: |  |p' ||
            echo "  (nothing published yet)"
        echo "cached here:"
        # shellcheck disable=SC2012  # names only; no odd filenames in a version dir
        ls -1t "$cache" 2>/dev/null | grep -vx kdeskdash-publisher | sed 's/^/  /' || echo "  (none)"
        for target in "$@"; do
            got=$(probe_version "$target")
            printf '%s: %s\n' "$target" "${got:-unreachable, not installed, or too old to say}"
        done
        ;;

    *)
        echo "unknown command: $cmd" >&2
        exit 2
        ;;
esac
