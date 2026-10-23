#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
DOCKPIPE_BIN="${DOCKPIPE_BIN:-$(command -v dockpipe || true)}"

if [[ ! -x "${DOCKPIPE_BIN}" ]]; then
  echo "packages/agent/tests/run.sh: missing ${DOCKPIPE_BIN}; set DOCKPIPE_BIN to an installed Dockpipe executable" >&2
  exit 1
fi

for workflow_path in \
  "${ROOT}/packages/agent/workflows/agent.cloud-lanes.doctor/config.yml"; do
  echo "--- validate ${workflow_path} ---"
  "${DOCKPIPE_BIN}" workflow validate "${workflow_path}"
done

echo "--- check promoted files exclude junk ---"
if find "${ROOT}/packages/agent" -type f \( \
  -path '*/bin/*' -o -path '*/obj/*' -o -path '*/dist/*' -o -path '*/build/*' -o \
  -path '*/node_modules/*' -o -path '*/.venv/*' -o -path '*/__pycache__/*' -o \
  -name '*.exe' -o -name '*.dll' -o -name '*.so' -o -name '*.dylib' -o \
  -name '*.zip' -o -name '*.tar' -o -name '*.7z' -o -name '*.log' -o -name '.env' \
  \) | grep -q .; then
  echo "packages/agent/tests/run.sh: unexpected generated or binary artifact under packages/agent" >&2
  exit 1
fi

echo "Portable package source checks passed; agent image qualification follows."

# Image/tooling tests belong to this package and must fail if Docker is unavailable.
bash "$ROOT/packages/agent/tests/integration/run.sh"
bash "$ROOT/packages/agent/tests/test_claude_profile.sh"

echo "packages/agent/tests/run.sh OK"
