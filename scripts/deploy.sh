#!/usr/bin/env bash
# Deploy / service management for kdeskdash on the Pi.
#
# Usage:
#   scripts/deploy.sh deploy          <target> <binary>
#   scripts/deploy.sh install-service <target> <service-file> <env-example>
#
# Invoked by the CMake `deploy` and `install-service` targets, but also safe to
# run by hand. Keeping the remote shell pipelines here (instead of inline in
# CMakeLists) avoids fragile nested-quote escaping through cmake -> make -> sh.
set -euo pipefail

cmd=${1:?usage: deploy.sh <deploy|install-service> ...}

case "$cmd" in
  deploy)
    target=${2:?missing ssh target}
    binary=${3:?missing binary path}
    # Stop the service if installed; fall back to killing a manually-run instance.
    # scp fails with "text file busy" if the binary is still running.
    ssh "$target" 'sudo systemctl stop kdeskdash 2>/dev/null || true; sudo pkill -INT kdeskdash 2>/dev/null || true; sleep 1'
    scp "$binary" "$target:~/kdeskdash"
    ssh "$target" 'sudo systemctl start kdeskdash 2>/dev/null || true'
    echo "deployed kdeskdash to $target"
    ;;

  install-service)
    target=${2:?missing ssh target}
    service=${3:?missing service file}
    env_example=${4:?missing env example file}
    scp "$service" "$env_example" "$target:~/"
    svc=$(basename "$service")
    env=$(basename "$env_example")
    # Install the unit + an env file (only if absent), reload, enable.
    ssh "$target" "
      set -e
      sudo install -D -m644 ~/$svc /etc/systemd/system/kdeskdash.service
      sudo install -d -m755 /etc/kdeskdash
      if [ ! -f /etc/kdeskdash/kdeskdash.env ]; then
        sudo install -m600 ~/$env /etc/kdeskdash/kdeskdash.env
      fi
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
