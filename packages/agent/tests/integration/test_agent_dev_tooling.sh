#!/usr/bin/env bash
# Test compiled Claude resolver image has Node and Claude CLI (no API call); validates image for AI workflows.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
source "$REPO_ROOT/workflows/lib/installed-dockpipe.sh"
DOCKPIPE="$DOCKPIPE_BIN"
unset DOCKPIPE_REPO_ROOT
cd "$REPO_ROOT"

# run.sh already compiles this package; do not request the legacy core-source closure.

# Node is available (the Claude image is Node-based)
out=$("$DOCKPIPE" --no-data --no-compile-deps --workflow claude -- node -e "console.log('node ok')")
[[ "$out" == *"node ok"* ]] || { echo "Expected node ok, got: $out"; exit 1; }

# Claude CLI is installed (we only check it exists; no API call)
out=$("$DOCKPIPE" --no-data --no-compile-deps --workflow claude -- which claude 2>/dev/null || true)
[[ "$out" == *"claude"* ]] || { echo "Expected claude in PATH, got: $out"; exit 1; }

echo "test_agent_dev_tooling OK"
