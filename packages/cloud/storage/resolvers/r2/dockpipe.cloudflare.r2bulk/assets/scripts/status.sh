#!/usr/bin/env bash
set -euo pipefail
script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec "${R2_BULK_PYTHON:-python3}" "$script_directory/r2-bulk.py" status
