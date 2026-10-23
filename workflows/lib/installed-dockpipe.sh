#!/usr/bin/env bash
# Shared installed-CLI selection for catalog maintainer workflows.
if [[ -v DOCKPIPE_BIN ]]; then
  if [[ "$DOCKPIPE_BIN" != /* || ! -x "$DOCKPIPE_BIN" ]]; then
    echo 'DOCKPIPE_BIN must name an absolute executable.' >&2
    return 1
  fi
else
  DOCKPIPE_BIN="$(command -v dockpipe)" || {
    echo 'Install Dockpipe or set DOCKPIPE_BIN to an absolute executable.' >&2
    return 1
  }
fi
export DOCKPIPE_BIN

dockpipe_cli() {
  "$DOCKPIPE_BIN" "$@"
}
