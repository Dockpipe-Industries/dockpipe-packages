#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
resolver_root="$(cd "$script_dir/../../../.." && pwd)"
helper="$resolver_root/assets/tooling/bin/cloudflare-edge"
if [[ ! -x "$helper" ]]; then
  echo 'The packaged lifecycle helper is unavailable; build the remote package source first.' >&2
  exit 1
fi
exec "$helper" lifecycle-change \
  --request "${DOCKPIPE_DEPENDENCY_REQUEST:?Private request is required}" \
  --operation "${DOCKPIPE_DEPENDENCY_OPERATION:?Explicit operation is required}" \
  --output "${DOCKPIPE_DEPENDENCY_OUTPUT:?Private result output is required}"
