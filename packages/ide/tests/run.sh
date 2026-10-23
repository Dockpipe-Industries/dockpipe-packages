#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
node "$ROOT/packages/ide/tests/devcontainer-lifecycle.test.js"
bash "$ROOT/packages/ide/tests/test_ide_state_ownership.sh"
bash "$ROOT/packages/ide/tests/test_cursor_dev_session_guard.sh"
