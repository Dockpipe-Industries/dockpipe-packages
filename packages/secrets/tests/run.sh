#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$root"
python3 "$root/tooling/catalog_go.py" -- test -p 1 ./packages/secrets/tools/...
