#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
resolver_root="$(cd "$script_dir/../../../.." && pwd)"
helper="$resolver_root/assets/tooling/bin/cloudflare-edge"
if [[ ! -x "$helper" ]]; then
  echo 'The packaged Cloudflare dependency inspector is unavailable; build the remote package source first.' >&2
  exit 1
fi
exec "$helper" lifecycle-inspect \
  --output "${DOCKPIPE_DEPENDENCY_OUTPUT:?Private report output is required}" \
  --machine "${DOCKPIPE_DEPENDENCY_MACHINE:?Machine identity is required}"
