#!/usr/bin/env bash
# Deploy / service management for kdeskdash on the Pi.
#
# Usage:
#   scripts/deploy.sh deploy          <target> <binary> [ttf]
#   scripts/deploy.sh install-service <target> <service-file> <env-file>
#
# Invoked by `just deploy <host>` / `just install-service <host>` (and by the
# legacy CMake targets of the same names), but also safe to run by hand. The
# target is any ssh destination, so one build serves every dashboard Pi.
# Keeping the remote shell pipelines here (instead of inline in CMakeLists)
# avoids fragile nested-quote escaping through cmake -> make -> sh.
set -euo pipefail

cmd=${1:?usage: deploy.sh <deploy|install-service> ...}

case "$cmd" in
  deploy)
    target=${2:?missing ssh target}
    binary=${3:?missing binary path}
    ttf=${4:-}   # optional: Symbols Nerd Font for the icons mode
    # Stop the service if installed; fall back to killing a manually-run instance.
    # scp fails with "text file busy" if the binary is still running.
    ssh "$target" 'sudo systemctl stop kdeskdash 2>/dev/null || true; sudo pkill -INT kdeskdash 2>/dev/null || true; sleep 1'
    # Stage in $HOME (scp has no sudo), then install to a root-owned path so the
    # root service does not execute a binary from a user-writable directory.
    scp "$binary" "$target:~/kdeskdash.new"
    ssh "$target" 'sudo install -m755 ~/kdeskdash.new /usr/local/bin/kdeskdash && rm -f ~/kdeskdash.new'
    # Icons-mode assets: the runtime font (install once — it is 2.4MB and rarely
    # changes) and the writable state dir for the favourites file. Both are
    # optional to the app (the mode shows an unavailable state without the font).
    if [ -n "$ttf" ]; then
      ssh "$target" 'sudo install -d -m755 /usr/local/share/kdeskdash /var/lib/kdeskdash'
      if ssh "$target" 'test ! -f /usr/local/share/kdeskdash/SymbolsNerdFont-Regular.ttf'; then
        scp "$ttf" "$target:~/kdeskdash-nf.ttf"
        ssh "$target" 'sudo install -D -m644 ~/kdeskdash-nf.ttf /usr/local/share/kdeskdash/SymbolsNerdFont-Regular.ttf && rm -f ~/kdeskdash-nf.ttf'
        echo "installed Nerd Font to $target:/usr/local/share/kdeskdash/"
      fi
    fi
    ssh "$target" 'sudo systemctl start kdeskdash 2>/dev/null || true'
    echo "deployed kdeskdash to $target:/usr/local/bin/kdeskdash"
    ;;

  install-service)
    target=${2:?missing ssh target}
    service=${3:?missing service file}
    env_file=${4:?missing env file}
    # A host with no committed deploy/hosts/<host>.env yet still gets a working
    # install: fall back to the full-surface example, which is all-defaults.
    if [ ! -f "$env_file" ]; then
      fallback="$(dirname "$service")/kdeskdash.env.example"
      echo "note: $env_file not found — installing $fallback instead" >&2
      echo "      (commit that file to give this host its own config)" >&2
      env_file="$fallback"
    fi
    scp "$service" "$env_file" "$target:~/"
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
    echo "installed kdeskdash service on $target"
    ;;

  *)
    echo "unknown command: $cmd" >&2
    exit 2
    ;;
esac
