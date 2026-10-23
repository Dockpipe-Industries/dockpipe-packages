#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
package_root="$(cd "$script_dir/../../../.." && pwd)"
exec python3 "$package_root/assets/scripts/pipelang.py" invoke "$@"
