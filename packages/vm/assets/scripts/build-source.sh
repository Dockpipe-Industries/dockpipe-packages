#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python="${DOCKPIPE_HOST_PYTHON_BIN-python3}"
if [[ -z "$python" ]]; then
  echo 'DOCKPIPE_HOST_PYTHON_BIN must not be explicitly empty.' >&2
  exit 1
fi
exec "$python" "$script_dir/build-source.py" "$@"
