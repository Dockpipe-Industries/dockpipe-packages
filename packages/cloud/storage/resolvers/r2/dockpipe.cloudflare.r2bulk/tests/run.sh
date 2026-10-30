#!/usr/bin/env bash
set -euo pipefail
package_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
python_runtime="${R2_BULK_PYTHON:-python3}"
# Release qualification requires the locked SDK; unittest skips cannot qualify it.
"$python_runtime" -B - "$package_root/requirements.lock" <<'PYTHON'
from importlib.metadata import PackageNotFoundError, version
from pathlib import Path
import re
import sys

for line in Path(sys.argv[1]).read_text().splitlines():
    match = re.match(r"^([A-Za-z0-9_-]+)==([^ \\]+)", line)
    if match is None:
        continue
    distribution, expected = match.groups()
    try:
        actual = version(distribution)
    except PackageNotFoundError:
        raise SystemExit(f"Missing locked dependency {distribution} {expected}")
    if actual != expected:
        raise SystemExit(f"Expected locked {distribution} {expected}, found {actual}")
PYTHON
"$python_runtime" -B -m unittest discover -s "$package_root/tests" -p 'test_*.py' -v
