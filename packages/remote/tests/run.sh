#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../../.." && pwd)"
cd "$repo_root"
python3 "$repo_root/tooling/catalog_go.py" -- test -p 1 ./packages/remote/tools/...
