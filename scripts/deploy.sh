#!/usr/bin/env bash
# Deploy / service management for kdeskdash on the Pi.
#
# Usage:
#   scripts/deploy.sh deploy          <target> [version]
#   scripts/deploy.sh install-service <target> <env-file> [version]
#   scripts/deploy.sh push-dev        <target> <binary> [ttf]
#   scripts/deploy.sh fetch           [version]
#   scripts/deploy.sh versions        <target>...
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

# Resolve `latest` (or take the version given), fetch the artifact into
# .deploy-cache/<version>/, verify every file against SHA256SUMS, and echo the
# directory. A cached copy that still verifies is reused — the store is
# immutable, so a version that checks out cannot have changed underneath us.
fetch_artifact() {
    local want=${1:-}
    : "${KDESKDASH_STORE_URL:?set KDESKDASH_STORE_URL in .env (e.g. https://kubsdb.encke-wahoo.ts.net:4880)}"
    local base="$KDESKDASH_STORE_URL/artifacts/kdeskdash"

    local v="$want"
    if [ -z "$v" ]; then
        v=$(curl -fsS "$base/latest") || die "no published versions at $base (has anything been published?)"
        v=$(printf '%s' "$v" | tr -d '[:space:]')
    fi

    local dir="$cache/$v"
    if [ -f "$dir/SHA256SUMS" ] && (cd "$dir" && sha256sum -c --quiet SHA256SUMS 2>/dev/null); then
        echo "using cached artifact $v" >&2
        printf '%s\n' "$dir"
        return
    fi

    echo "fetching kdeskdash $v from the store" >&2
    rm -rf "$dir"
    mkdir -p "$dir"
    curl -fsS -o "$dir/SHA256SUMS" "$base/$v/SHA256SUMS" ||
        die "no artifact for version '$v' in the store (try: just versions)"
    # Drive the download off SHA256SUMS rather than a hardcoded file list, so an
    # older version that shipped a different set of files still fetches whole.
    local sum name
    while read -r sum name; do
        [ -n "${sum:-}" ] || continue
        curl -fsS -o "$dir/$name" "$base/$v/$name" || die "fetch failed: $name"
    done < "$dir/SHA256SUMS"
    (cd "$dir" && sha256sum -c --quiet SHA256SUMS) || die "checksum mismatch in $v — refusing to deploy it"

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

cmd=${1:?usage: deploy.sh <deploy|install-service|push-dev|fetch|versions> ...}
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
        echo "installed kdeskdash service on $target (unit from $v)"
        ;;

    fetch)
        fetch_artifact "${1:-}"
        ;;

    versions)
        : "${KDESKDASH_STORE_HOST:?set KDESKDASH_STORE_HOST in .env (the host running kpkg, e.g. kubsdb)}"
        echo "store:"
        ssh -n "$KDESKDASH_STORE_HOST" 'kpkg list' | sed -n 's|^artifacts/kdeskdash: |  |p' ||
            echo "  (nothing published yet)"
        echo "cached here:"
        # shellcheck disable=SC2012  # names only; no odd filenames in a version dir
        ls -1t "$cache" 2>/dev/null | sed 's/^/  /' || echo "  (none)"
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
