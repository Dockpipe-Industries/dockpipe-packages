#!/usr/bin/env bash
set -euo pipefail

PACKAGE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$PACKAGE_ROOT/../.." && pwd)"
source "$REPO_ROOT/workflows/lib/installed-dockpipe.sh"
SCRIPT="$PACKAGE_ROOT/resolvers/cursor-dev/assets/scripts/cursor-dev-session.sh"
[[ -f "$SCRIPT" ]] || { echo "Missing package-owned Cursor session script: $SCRIPT" >&2; exit 1; }
export TEST_DOCKPIPE_BIN="$DOCKPIPE_BIN"

test_existing_session_guard() (
  local tmp fakebin workdir docker_log pid out rc script_dir state_root cursor_state
  umask 077
  tmp="$(mktemp -d)"
  pid=""
  trap '
    if [[ -n "$pid" ]]; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
    rm -rf "$tmp"
  ' EXIT
  export XDG_STATE_HOME="${tmp}/state"
  # This fixture owns a new workspace rather than the parent workflow state.
  unset DOCKPIPE_STATE_DIR
  export DOCKPIPE_PACKAGE_ROOT="${REPO_ROOT}/packages/ide"
  export DOCKPIPE_PACKAGE_MANIFEST="${DOCKPIPE_PACKAGE_ROOT}/package.yml"
  fakebin="${tmp}/bin"
  workdir="${tmp}/work"
  docker_log="${tmp}/docker.log"
  mkdir -p "${workdir}"
  state_root="$("$DOCKPIPE_BIN" get state_dir --workdir "$workdir")"
  mkdir -p "$state_root"
  cursor_state="$("$DOCKPIPE_BIN" __state package-runtime --workdir "$workdir" --owner ide/resolver/cursor-dev --ensure-private)"
  mkdir -p "${fakebin}" "${cursor_state}"
  script_dir="$(dirname "${SCRIPT}")"

  cat >"${fakebin}/docker" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
log_file="${FAKE_DOCKER_LOG:?}"
printf '%s\n' "$*" >>"$log_file"
case "${1:-}" in
  info)
    exit 0
    ;;
  image)
    if [[ "${2:-}" == "inspect" ]]; then
      exit 0
    fi
    ;;
  inspect)
    if [[ "${2:-}" == "-f" ]]; then
      printf 'running\n'
      exit 0
    fi
    ;;
  run)
    echo "unexpected docker run" >&2
    exit 99
    ;;
esac
echo "unexpected docker invocation: $*" >&2
exit 98
EOF
  cat >"${fakebin}/dockpipe" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
exec "${TEST_DOCKPIPE_BIN:?}" "$@"
EOF
  chmod +x "${fakebin}/docker" "${fakebin}/dockpipe"

  sleep 60 &
  pid=$!
  cat >"${cursor_state}/active-session.env" <<EOF
CURSOR_DEV_ACTIVE_PID=${pid}
CURSOR_DEV_ACTIVE_CONTAINER=existing-container
CURSOR_DEV_ACTIVE_WORKDIR=${workdir}
EOF

  out="${tmp}/out.log"
  cd "$workdir"
  set +e
  FAKE_DOCKER_LOG="${docker_log}" \
    DOCKPIPE_SCRIPT_DIR="${script_dir}" \
    DOCKPIPE_WORKDIR="${workdir}" \
    PATH="${fakebin}:$PATH" \
    bash "${SCRIPT}" >"${out}" 2>&1
  rc=$?
  set -e

  kill "${pid}" 2>/dev/null || true
  wait "${pid}" 2>/dev/null || true
  pid=""

  if [[ ${rc} -ne 0 ]]; then
    echo "test_existing_session_guard FAIL: expected zero exit, got ${rc}"
    cat "${out}"
    return 1
  fi
  if ! grep -q "An active session is already registered for this workdir" "${out}"; then
    echo "test_existing_session_guard FAIL: missing active-session warning"
    cat "${out}"
    return 1
  fi
  if grep -q '^run ' "${docker_log}" || grep -q 'unexpected docker run' "${out}"; then
    echo "test_existing_session_guard FAIL: docker run should not happen when guard triggers"
    cat "${docker_log}"
    cat "${out}"
    return 1
  fi

  echo "test_existing_session_guard OK"
)

run_tests() {
  test_existing_session_guard
}

run_tests
