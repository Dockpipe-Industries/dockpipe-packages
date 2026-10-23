#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../../../.." && pwd)"
source "$repo_root/workflows/lib/installed-dockpipe.sh"
unset DOCKPIPE_REPO_ROOT
"$DOCKPIPE_BIN" package compile workflows --workdir "$repo_root" --from "$repo_root/packages/agent" --force
"$DOCKPIPE_BIN" package compile resolvers --workdir "$repo_root" --from "$repo_root/packages/agent" --force
command -v docker >/dev/null || { echo 'Agent image qualification requires Docker.' >&2; exit 1; }
docker info >/dev/null || { echo 'Agent image qualification requires an available Docker daemon.' >&2; exit 1; }
for test_script in "$script_dir"/test_*.sh; do
  bash "$test_script"
done
