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
"$dockpipe_bin" package test --workdir "$repo_root"
python3 -B -m unittest discover -s "$repo_root/workflows/package/package-store-staging-infra/tests" -p 'test_*.py'
python3 -B -m unittest discover -s "$repo_root/workflows/package/package-release-setup/tests" -p 'test_*.py'
