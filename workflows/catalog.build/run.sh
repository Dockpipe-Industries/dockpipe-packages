#!/usr/bin/env bash
set -euo pipefail
workflow_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$workflow_root/../.." && pwd)"
if [[ -v DOCKPIPE_BIN ]]; then
  dockpipe_bin="$DOCKPIPE_BIN"
  if [[ "$dockpipe_bin" != /* || ! -x "$dockpipe_bin" ]]; then
    echo 'DOCKPIPE_BIN must be an absolute executable path.' >&2
    exit 1
  fi
else
  dockpipe_bin="$(command -v dockpipe)" || {
    echo 'Install Dockpipe or set DOCKPIPE_BIN.' >&2
    exit 1
  }
fi
export DOCKPIPE_BIN="$dockpipe_bin"
"$dockpipe_bin" package compile resolvers --workdir "$repo_root" --from "$repo_root/packages" --force
"$dockpipe_bin" package compile workflows --workdir "$repo_root" --from "$repo_root/packages" --force
# Package source hooks finalize resolver artifacts with their native tools.
"$dockpipe_bin" package build source --workdir "$repo_root"
