#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
source "$repo_root/workflows/lib/installed-dockpipe.sh"
unset DOCKPIPE_REPO_ROOT
container_auth_dir="$("$DOCKPIPE_BIN" scope resolver claude container-auth-dir --workdir "$repo_root")"
auth_mount_mode="$("$DOCKPIPE_BIN" scope resolver claude auth-mount-mode --workdir "$repo_root")"
[[ "$container_auth_dir" == /home/node/.claude ]] || { echo 'Claude resolver lost its declared container auth directory.' >&2; exit 1; }
[[ "$auth_mount_mode" == rw ]] || { echo 'Claude resolver lost its declared auth mount mode.' >&2; exit 1; }
echo 'Claude package profile uses the installed Dockpipe resolver contract.'
