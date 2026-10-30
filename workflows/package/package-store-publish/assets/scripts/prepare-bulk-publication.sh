#!/usr/bin/env bash
set -euo pipefail
root="${DOCKPIPE_SOURCE_ROOT:-$(pwd)}"
source "$root/workflows/lib/installed-dockpipe.sh"
output="$(dockpipe_cli scope artifacts publication)"
environment_output="$(dockpipe_cli scope artifacts bulk-publication.env)"
script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
python3 "$script_directory/publication.py" prepare --output "$output" --environment-output "$environment_output"
