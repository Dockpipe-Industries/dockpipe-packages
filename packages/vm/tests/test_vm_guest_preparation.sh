#!/usr/bin/env bash
set -euo pipefail
repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
script="${DOCKPIPE_VMIMAGE_TEST_SCRIPT:-$repository_root/resolvers/qemu/assets/scripts/vmimage-run.sh}"
fixture="$(mktemp -d)"
trap 'rm -rf "$fixture"' EXIT
# Sourcing the shared SDK may query package state through a discoverable CLI.
# Keep that setup inside this test even when launched from an owning checkout.
for directory in home state cache config project global internal packages; do
  mkdir -m 700 "$fixture/$directory"
done
export HOME="$fixture/home"
export XDG_STATE_HOME="$fixture/state"
export XDG_CACHE_HOME="$fixture/cache"
export XDG_CONFIG_HOME="$fixture/config"
export DOCKPIPE_WORKDIR="$fixture/project"
export DOCKPIPE_GLOBAL_ROOT="$fixture/global"
export DOCKPIPE_STATE_DIR="$fixture/internal"
export DOCKPIPE_PACKAGES_ROOT="$fixture/packages"
export DOCKPIPE_VMIMAGE_SOURCE_ONLY=1
# shellcheck disable=SC1090
source "$script"
unset DOCKPIPE_VM_GUEST_PREPARATION DOCKPIPE_RESOLVER_VM_GUEST_PREPARATION
unset DOCKPIPE_VM_WINDOWS_INSTALL_MODE
export DOCKPIPE_VM_BOOT_SOURCE=installer-iso
export DOCKPIPE_VM_CDROM="$fixture/installer.iso"
export DOCKPIPE_VM_DISK="$fixture/destination.qcow2"
export DOCKPIPE_VM_DISK_SIZE=64G
printf 'real installer fixture\n' > "$DOCKPIPE_VM_CDROM"

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

vmimage_prompt_optional_file_value() {
  printf '%s\n' "$1" >> "$fixture/optional-prompts"
}

vmimage_prompt_file_value() {
  printf '%s\n' "$1" >> "$fixture/required-prompts"
  return 71
}

vmimage_prompt_required_input() {
  printf '%s\n' "$1" >> "$fixture/required-prompts"
  return 71
}

[[ "$(vmimage_guest_preparation)" == automatic ]] || fail 'Legacy default changed'
vmimage_ensure_prompted_inputs
[[ "$(cat "$fixture/optional-prompts")" == DOCKPIPE_VM_VIRTIO_ISO ]] || fail 'Legacy optional prompt lost'
rm "$fixture/optional-prompts"
export DOCKPIPE_RESOLVER_VM_GUEST_PREPARATION=provided-only
[[ "$(vmimage_guest_preparation)" == provided-only ]] || fail 'Resolver fallback ignored'
vmimage_ensure_prompted_inputs
[[ ! -e "$fixture/optional-prompts" ]] || fail 'Absent optional media prompted'
set +e
(
  set -e
  unset DOCKPIPE_VM_CDROM
  vmimage_ensure_prompted_inputs
)
required_status=$?
set -e
[[ "$required_status" -eq 71 ]] || fail 'Missing required installer ISO accepted'
[[ "$(cat "$fixture/required-prompts")" == DOCKPIPE_VM_CDROM ]] || fail 'Required ISO prompt lost'
rm "$fixture/required-prompts"
export DOCKPIPE_VM_GUEST_PREPARATION=automatic
[[ "$(vmimage_guest_preparation)" == automatic ]] || fail 'Explicit runtime policy precedence lost'

# Invalid policy must stop every backend before any host check, state or child.
vmimage_host_os() {
  printf 'host check reached\n' >> "$fixture/effects"
  printf 'linux\n'
}
vmimage_require_host_dependencies() {
  printf 'dependencies reached\n' >> "$fixture/effects"
  return 73
}
vmimage_prepare_state_split() {
  printf 'state reached\n' >> "$fixture/effects"
  return 73
}
for entry in vmimage_run_qemu_kvm vmimage_run_qemu_darwin vmimage_run_qemu_windows; do
  # Each child intentionally receives a different policy without changing parent state.
  # shellcheck disable=SC2030
  if (
    export DOCKPIPE_VM_GUEST_PREPARATION=invalid
    "$entry"
  ) > "$fixture/invalid.stdout" 2> "$fixture/invalid.stderr"; then
    fail "Invalid policy accepted by $entry"
  fi
  grep -q 'unsupported DOCKPIPE_VM_GUEST_PREPARATION=invalid' "$fixture/invalid.stderr"
  [[ ! -e "$fixture/effects" ]] || fail "Invalid policy caused effects in $entry"
done
# shellcheck disable=SC2030,SC2031
if (
  export DOCKPIPE_VM_GUEST_PREPARATION=provided-only
  export DOCKPIPE_VM_WINDOWS_INSTALL_MODE=unattended
  vmimage_ensure_prompted_inputs
) > "$fixture/conflict.stdout" 2> "$fixture/conflict.stderr"; then
  fail 'Conflicting unattended builtin generation accepted'
fi
grep -q 'cannot generate unattended installer bootstrap' "$fixture/conflict.stderr"
[[ ! -e "$fixture/optional-prompts" && ! -e "$fixture/required-prompts" ]] || fail 'Conflict prompted first'

# Use actual payload-copy/bootstrap functions, with only owned source paths and
# a recording resolver-tool seam. Reserved names must remain package bytes.
vmimage_backend() {
  printf 'qemu-kvm\n'
}
vmimage_windows_bootstrap_dir() {
  printf '%s\n' "${fixture:?}/media"
}
vmimage_resolve_path() {
  printf '%s\n' "$1"
}
vmimage_shell_path() {
  printf '%s\n' "$1"
}
vmimage_builtin_bootstrap_script() {
  printf '%s\n' "$fixture/provision-windows-ssh.ps1"
}
vmimage_stage_resolver_guest_tools() {
  printf 'staged\n' >> "$fixture/tools-staged"
  mkdir -p "$1/GuestTools"
  printf 'implicit tools\n' > "$1/GuestTools/application.exe"
}
printf 'builtin fixture\n' > "$fixture/provision-windows-ssh.ps1"
# This parent assignment intentionally replaces policies scoped to child cases.
# shellcheck disable=SC2031
export DOCKPIPE_VM_GUEST_PREPARATION=provided-only
unset DOCKPIPE_VM_BOOTSTRAP_PATH
[[ -z "$(vmimage_prepare_bootstrap_media)" ]] || fail 'Absent payload produced media'
[[ ! -e "${fixture:?}/media" && ! -e "$fixture/tools-staged" ]] || fail 'Absent payload created files'
mkdir "$fixture/seed"
printf '#cloud-config\nhostname: disposable\n' > "$fixture/seed/user-data"
printf 'instance-id: task-only\n' > "$fixture/seed/meta-data"
printf 'package-owned reserved name\n' > "$fixture/seed/provision-windows-ssh.ps1"
export DOCKPIPE_VM_BOOTSTRAP_PATH="$fixture/seed"
[[ "$(vmimage_prepare_bootstrap_media)" == "${fixture:?}/media" ]] || fail 'Explicit payload path lost'
diff -r "$fixture/seed" "${fixture:?}/media"
[[ "$(vmimage_attach_bootstrap_media_dir "$fixture/media")" == "$fixture/media" ]] || fail 'Explicit seed attachment lost'
[[ "$(vmimage_bootstrap_attachment_mode installer-iso "$fixture/media")" == disk ]] || fail 'Installer seed attachment mode lost'
[[ "$(vmimage_bootstrap_attachment_mode image "$fixture/media")" == disk ]] || fail 'Image seed attachment mode lost'
[[ ! -e "$fixture/tools-staged" ]] || fail 'Implicit resolver tools injected'
rm -rf "${fixture:?}/media"
printf 'single explicit seed\n' > "$fixture/single-seed"
export DOCKPIPE_VM_BOOTSTRAP_PATH="$fixture/single-seed"
vmimage_prepare_bootstrap_media > "$fixture/single.path"
cmp "$fixture/single-seed" "$fixture/media/single-seed"
[[ "$(find "${fixture:?}/media" -type f | wc -l)" -eq 1 ]] || fail 'Single payload acquired extra files'
rm -rf "${fixture:?}/media"
export DOCKPIPE_VM_BOOTSTRAP_PATH="$fixture/missing"
if (vmimage_prepare_bootstrap_media) > "$fixture/missing.stdout" 2> "$fixture/missing.stderr"; then
  fail 'Missing explicit payload accepted'
fi
grep -q 'configured DOCKPIPE_VM_BOOTSTRAP_PATH does not exist' "$fixture/missing.stderr"
rm -rf "${fixture:?}/media"
unset DOCKPIPE_VM_BOOTSTRAP_PATH
export DOCKPIPE_VM_GUEST_PREPARATION=automatic
vmimage_prepare_bootstrap_media > "$fixture/default.path"
cmp "$fixture/provision-windows-ssh.ps1" "$fixture/media/provision-windows-ssh.ps1"
[[ -f "$fixture/media/Install-Dockpipe-Guest-Tools.cmd" ]] || fail 'Legacy bootstrap launcher lost'
[[ -f "$fixture/media/GuestTools/application.exe" ]] || fail 'Legacy resolver tools lost'
[[ "$(cat "$fixture/tools-staged")" == staged ]] || fail 'Legacy tools staging count changed'
printf 'Guest preparation contracts passed\n'
