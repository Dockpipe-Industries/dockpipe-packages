#!/usr/bin/env bash
set -euo pipefail

package_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fixture="$(mktemp -d)"
trap 'rm -rf "$fixture"' EXIT
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
source "$package_root/resolvers/qemu/assets/scripts/vmimage-run.sh"

export DOCKPIPE_VM_GUEST_PREPARATION=provided-only
export DOCKPIPE_VM_BOOT_SOURCE=installer-iso
export DOCKPIPE_VM_CDROM="$fixture/install.iso"
export DOCKPIPE_VM_DISK_SIZE=64G
unset DOCKPIPE_VM_DISK_FORMAT DOCKPIPE_VM_DISK_CREATE_FORMAT
unset DOCKPIPE_VM_WINDOWS_INSTALL_MODE DOCKPIPE_VM_SSH_USER
printf 'installer fixture\n' > "$DOCKPIPE_VM_CDROM"

expect_failure() {
  local expected="$1"
  shift
  if ("$@") > "$fixture/failure.stdout" 2> "$fixture/failure.stderr"; then
    printf 'Expected failure: %s\n' "$expected" >&2
    exit 1
  fi
  grep -Fq "$expected" "$fixture/failure.stderr"
}

vmimage_qemu_img_bin() {
  printf '%s\n' "$fixture/qemu-img"
}
vmimage_qemu_media_path() {
  printf '%s\n' "$1"
}
vmimage_confirm_prompts_enabled() {
  return 0
}
vmimage_prompt_confirm() {
  if [[ "$1" == vmimage.persistent-disk ]]; then
    [[ "$4" == no ]] || return 1
  fi
  printf '%s\n' "$1" >> "$fixture/prompts"
  printf '%s\n' "${TEST_CONFIRM_ANSWER:-yes}"
}
cat > "$fixture/qemu-img" <<'RECORDER'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$@" > "${0%/*}/create-arguments"
if [[ "${TEST_CREATE_FAIL:-}" == yes ]]; then
  exit 72
fi
if [[ -n "${TEST_COMPETING_DIRECTORY:-}" ]]; then
  mkdir "$TEST_COMPETING_DIRECTORY"
fi
if [[ -n "${TEST_COMPETING_DESTINATION:-}" ]]; then
  printf 'competing contents\n' > "$TEST_COMPETING_DESTINATION"
fi
printf 'fixture format=%s size=%s\n' "$3" "$5" > "$4"
RECORDER
chmod +x "$fixture/qemu-img"

for extension in qcow2 raw img; do
  export DOCKPIPE_VM_DISK="$fixture/new.$extension"
  vmimage_validate_installer_inputs
  vmimage_ensure_disk_exists_for_install "$DOCKPIPE_VM_DISK"
  expected_format=raw
  if [[ "$extension" == qcow2 ]]; then
    expected_format=qcow2
  fi
  grep -Fxq "fixture format=$expected_format size=64G" "$DOCKPIPE_VM_DISK"
  [[ "$(vmimage_base_format "$DOCKPIPE_VM_DISK")" == "$expected_format" ]]
done

# Reuse preserves contents and never invokes creation, even with an unused size.
rm "$fixture/create-arguments"
export DOCKPIPE_VM_DISK="$fixture/new.qcow2"
export DOCKPIPE_VM_DISK_SIZE=unused-for-existing-disk
vmimage_validate_installer_inputs
export TEST_CONFIRM_ANSWER=no
expect_failure 'stopped before modifying persistent VM disk' vmimage_ensure_disk_exists_for_install "$DOCKPIPE_VM_DISK"
[[ ! -e "$fixture/create-arguments" ]]
export TEST_CONFIRM_ANSWER=yes
vmimage_ensure_disk_exists_for_install "$DOCKPIPE_VM_DISK"
grep -Fxq 'fixture format=qcow2 size=64G' "$DOCKPIPE_VM_DISK"
grep -Fxq vmimage.persistent-disk "$fixture/prompts"

export DOCKPIPE_VM_DISK="$fixture/uncreated.qcow2"
for size in 0 0G -1G invalid 64GB '1;false'; do
  export DOCKPIPE_VM_DISK_SIZE="$size"
  expect_failure 'new disk size must be positive' vmimage_validate_installer_inputs
done
export DOCKPIPE_VM_DISK_SIZE=1.5G
vmimage_validate_installer_inputs
export DOCKPIPE_VM_DISK_CREATE_FORMAT=raw
expect_failure 'disk creation format conflicts' vmimage_validate_installer_inputs
unset DOCKPIPE_VM_DISK_CREATE_FORMAT
mkdir "$fixture/directory.qcow2"
export DOCKPIPE_VM_DISK="$fixture/directory.qcow2"
expect_failure 'installation disk must be' vmimage_validate_installer_inputs
export DOCKPIPE_VM_DISK="$DOCKPIPE_VM_CDROM"
expect_failure 'must be different files' vmimage_validate_installer_inputs
export DOCKPIPE_VM_DISK="$fixture/uncreated.qcow2"
export DOCKPIPE_VM_CDROM="$fixture/missing.iso"
expect_failure 'installer ISO must be' vmimage_validate_installer_inputs
export DOCKPIPE_VM_CDROM="$fixture/directory.qcow2"
expect_failure 'installer ISO must be' vmimage_validate_installer_inputs
export DOCKPIPE_VM_CDROM="$fixture/install.iso"
export DOCKPIPE_VM_BOOT_SOURCE=typo
expect_failure 'unsupported DOCKPIPE_VM_BOOT_SOURCE' vmimage_boot_source
export DOCKPIPE_VM_BOOT_SOURCE=installer-iso
export DOCKPIPE_VM_DISK_SIZE=64G

export TEST_CONFIRM_ANSWER=no
expect_failure 'stopped before creating VM disk' vmimage_ensure_disk_exists_for_install "$DOCKPIPE_VM_DISK"
[[ ! -e "$DOCKPIPE_VM_DISK" ]]
export TEST_CONFIRM_ANSWER=yes
export TEST_CREATE_FAIL=yes
if (vmimage_ensure_disk_exists_for_install "$DOCKPIPE_VM_DISK"); then
  echo 'Failed creation was accepted' >&2
  exit 1
fi
unset TEST_CREATE_FAIL
[[ ! -e "$DOCKPIPE_VM_DISK" ]]
[[ -z "$(find "$fixture" -name '*.dockpipe-create.*' -print)" ]]
export TEST_COMPETING_DESTINATION="$DOCKPIPE_VM_DISK"
expect_failure 'destination appeared during creation' vmimage_ensure_disk_exists_for_install "$DOCKPIPE_VM_DISK"
grep -Fxq 'competing contents' "$DOCKPIPE_VM_DISK"
[[ -z "$(find "$fixture" -name '*.dockpipe-create.*' -print)" ]]
unset TEST_COMPETING_DESTINATION
export DOCKPIPE_VM_DISK="$fixture/competing-directory.qcow2"
export TEST_COMPETING_DIRECTORY="$DOCKPIPE_VM_DISK"
expect_failure 'destination appeared during creation' vmimage_ensure_disk_exists_for_install "$DOCKPIPE_VM_DISK"
[[ -d "$DOCKPIPE_VM_DISK" ]]
[[ -z "$(find "$DOCKPIPE_VM_DISK" -mindepth 1 -print)" ]]
[[ -z "$(find "$fixture" -name '*.dockpipe-create.*' -print)" ]]
unset TEST_COMPETING_DIRECTORY

# Exercise the real argument assembler. Host dependencies, state integration and
# guest execution are inert; disk checks, preparation and branch selection remain real.
for operation in require_host_dependencies prepare_state_split ensure_secure_boot_firmware \
  confirm_user_supplied_media_rights confirm_host_network_exposure validate_pci_passthrough \
  prepare_session_event report_session; do
  eval "vmimage_${operation}() {
    :
  }"
done
vmimage_prepare_state_split() {
  printf 'state prepared\n' >> "$fixture/state-prepared"
}
vmimage_qemu_bin() {
  printf '/unused-qemu\n'
}
vmimage_prepare_socket_dir() {
  export DOCKPIPE_VM_SOCKET_DIR="$fixture"
}
vmimage_state_dir() {
  printf '%s\n' "$fixture"
}
vmimage_machine_uuid() {
  printf '4e03054d-4449-4564-b903-38a385071a08\n'
}
vmimage_net_mac() {
  printf '52:54:00:12:34:56\n'
}
vmimage_disk_serial() {
  printf 'installer-fixture\n'
}
vmimage_clipboard_mode() {
  printf 'off\n'
}
vmimage_agent_enabled() {
  return 1
}
vmimage_run_installer_session() {
  shift
  printf '%s\0' "$@" > "$fixture/arguments"
}
vmimage_remote_run_internal() {
  echo 'Unexpected SSH execution' >&2
  exit 91
}
vmimage_windows_ensure_ssh_key() {
  printf 'requested\n' >> "$fixture/windows-key"
}
export DOCKPIPE_VM_TPM=off DOCKPIPE_VM_SECURE_BOOT=off
export DOCKPIPE_VM_DISK="$fixture/new.qcow2"
export DOCKPIPE_VM_PERSISTENCE=ephemeral
export DOCKPIPE_VM_SSH_USER=linuxuser
export DOCKPIPE_STEP_CMD='echo must-not-run-during-install'
unset DOCKPIPE_VM_INTERACTIVE DOCKPIPE_VM_INTERACTIVE_SSH
unset DOCKPIPE_VM_FIRMWARE_CODE DOCKPIPE_VM_FIRMWARE_VARS DOCKPIPE_VM_BIOS
unset DOCKPIPE_VM_VIRTIO_ISO DOCKPIPE_VM_BOOTSTRAP_PATH DOCKPIPE_VM_WINDOWS_UNATTEND_DIR
unset DOCKPIPE_VM_HOSTFWD DOCKPIPE_VM_PCI_DEVICES DOCKPIPE_VM_GPU_PRIMARY
export DOCKPIPE_VM_CDROM="$fixture/directory.qcow2"
expect_failure 'installer ISO must be' vmimage_run_qemu_common qemu-kvm
[[ ! -e "$fixture/state-prepared" ]]
export DOCKPIPE_VM_CDROM="$fixture/install.iso"
vmimage_run_qemu_common qemu-kvm
[[ "$DOCKPIPE_VM_PERSISTENCE" == persistent ]]
[[ ! -e "$fixture/windows-key" ]]
python3 - "$fixture/arguments" "$DOCKPIPE_VM_DISK" "$DOCKPIPE_VM_CDROM" <<'ARGS'
from pathlib import Path
import sys
arguments = Path(sys.argv[1]).read_bytes().rstrip(b'\0').decode().split('\0')
assert arguments[arguments.index('-boot') + 1] == 'once=d,menu=on'
assert arguments[arguments.index('-cdrom') + 1] == sys.argv[3]
assert arguments[arguments.index('-display') + 1] == 'gtk,window-close=on'
assert any(f'file={sys.argv[2]},format=qcow2' in argument for argument in arguments)
assert '-daemonize' not in arguments
assert not any('vbootstrap' in argument for argument in arguments)
ARGS

# Prepared images must not invent credentials under provided-only. Automatic
# retains the legacy Windows preparation path. Keep both sessions interactive.
export DOCKPIPE_VM_BOOT_SOURCE=image DOCKPIPE_VM_INTERACTIVE=1
export DOCKPIPE_VM_PERSISTENCE=persistent
unset DOCKPIPE_VM_CDROM
vmimage_run_qemu_common qemu-kvm
[[ ! -e "$fixture/windows-key" ]]
# Record the legacy bootstrap boundary without producing tools or credentials.
vmimage_prepare_bootstrap_media() {
  :
}
export DOCKPIPE_VM_GUEST_PREPARATION=automatic
vmimage_run_qemu_common qemu-kvm
[[ "$(cat "$fixture/windows-key")" == requested ]]
printf '%s\n' 'Linux installer input, disk, session and preparation contracts passed'
