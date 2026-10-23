#!/usr/bin/env bash
set -euo pipefail
ROOT="${DOCKPIPE_SOURCE_ROOT:-${DOCKPIPE_WORKDIR:-$(pwd)}}"
source "$ROOT/workflows/lib/installed-dockpipe.sh"
ARTIFACTS_DIR="${RELEASE_ARTIFACTS_DIR:-release/artifacts}"
VERSION="${RELEASE_VERSION:-$(tr -d ' \t\r\n' < "$ROOT/VERSION")}"
mkdir -p "$ROOT/$ARTIFACTS_DIR"
dockpipe_cli --workflow-file "$ROOT/workflows/catalog.build/config.yml" --
dockpipe_cli package build store --workdir "$ROOT" --out "$ROOT/$ARTIFACTS_DIR" --version "$VERSION"
printf '[dockpipe-packages] artifacts ready in %s/%s\n' "$ROOT" "$ARTIFACTS_DIR" >&2
