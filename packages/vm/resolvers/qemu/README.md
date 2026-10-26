# qemu

Packaged QEMU resolver for the `vm` runtime family.

The adjacent `QemuVmResolverConfig` preserves the Windows defaults and current
`DOCKPIPE_VM_*` bindings. The Linux workflow owns its `LinuxQemuVmResolverConfig`
and related form models under `workflows/linux-vm/models`, so its compiled archive
can resolve typed inputs independently of the resolver archive's cache location.
Both workflows compose the generic `vm` runtime; package-owned qualification
planning does not change the runtime or execute QEMU.

What it owns:

- concrete VMM backend selection for the VM substrate
- Windows-friendly QEMU defaults such as disk bus and NIC model
- baseline guest hardware defaults for the first-party Windows flow
- optional host-to-guest sync settings consumed by the generic vmimage runner before the guest command starts
- optional Dockpipe guest agent forwarding for structured status and shutdown on provisioned Windows guests
- host-side delegation for the `vm` runtime; workflows should select `runtime: vm` and `resolver: qemu` and let the resolver own the host bridge

What it does not own:

- Windows media or licenses
- Dockpipe core VM lifecycle primitives
- workflow-specific Windows setup values like locale, admin user, or guest command

Current resolver defaults:

- backend: `qemu-kvm`
- disk bus: `sata`
- network adapter: `e1000e`
- TPM: `required`
- secure boot: `required`
- CPUs / memory: `4` / `8G`

Workflow shape:

- use `runtime: vm`
- use `resolver: qemu`
- do not set `kind: host` on steps that use or inherit this resolver

Minimal pattern:

```yaml
runtime: vm
resolver: qemu

steps:
  - id: guest-command
    vm:
      guest_path: C:\uh
      keepalive: true
    cmd: Set-Location C:\uh; whoami; hostname
```

Multiple mount pattern:

```yaml
runtime: vm
resolver: qemu

steps:
  - id: guest-command
    vm:
      mounts:
        - host: C:\Source\worktrees\uh1
          guest: C:\uh
        - host: C:\tmp\artifacts
          guest: C:\artifacts
      keepalive: true
    cmd: Set-Location C:\uh; whoami; hostname
```

Interactive SSH pattern:

```yaml
runtime: vm
resolver: qemu

steps:
  - id: guest-shell
    vm:
      interactive_ssh: true
      guest_path: C:\uh
      keepalive: true
```

Interactive debug pattern:

```yaml
runtime: vm
resolver: qemu

steps:
  - id: guest-debug
    vm:
      interactive_debug: true
    cmd: echo skipped while you inspect the guest manually
```

When `vm.interactive_debug: true` is set, Dockpipe tells the VM runner to launch a visible guest session and skip the normal guest-command-over-SSH automation path. Use it when you need to finish manual Windows setup before turning automation back on.

When `vm.interactive_ssh: true` is set, Dockpipe waits for guest SSH readiness and then opens a live authenticated shell instead of executing `cmd:` as a one-shot guest command. On Windows guests, Dockpipe currently uses a minimal `cmd.exe /d /q /k prompt $P$G` shell even when `ExecMode` is `powershell`, because that path is more stable over the password-authenticated `plink` terminal. Bash mode still opens `bash -li`.

If the guest image is not yet SSH-ready, the installed Dockpipe core runtime supplies the built-in `provision-windows-ssh.ps1` helper. It is staged from the installed core assets; this catalog does not ship a second copy.

For first-run setup before SSH exists, the VM runner now auto-attaches guest-readable bootstrap media containing that helper plus `dockpipe-guest-agent.exe`. If you also set `Advanced.BootstrapPath` / `DOCKPIPE_VM_BOOTSTRAP_PATH`, Dockpipe merges your extra host file or directory into the same bootstrap media. The filenames `provision-windows-ssh.ps1`, `dockpipe-guest-agent.exe`, `dockpipe-guest-agent.ps1`, and `README.txt` are reserved by Dockpipe and are always restaged last so a custom payload cannot accidentally replace them. This is the intended path for provisioning scripts and one-off setup assets when clipboard or sync are not available yet. The provisioning script installs the guest agent as a LocalSystem startup task.

When `Advanced.Agent` / `DOCKPIPE_VM_AGENT=true` is enabled, Dockpipe also forwards a guest agent port and can use that control plane for structured readiness and graceful shutdown once the guest has been provisioned.

When `Network.SshPassword` / `DOCKPIPE_VM_SSH_PASSWORD` is set on a Windows host, Dockpipe can use a non-interactive PuTTY transport (`plink` / `pscp`) instead of the default key-oriented OpenSSH path. This is intended as a pragmatic bootstrap mode; key auth still remains supported.

For visible Windows-host sessions, Dockpipe now defaults the display backend to `gtk,grab-on-hover=on,window-close=on` because the plain QEMU window default tends to release the pointer too easily at the edges.

When the Dockpipe guest agent is enabled and provisioned on a Windows guest, Dockpipe bridges plain-text clipboard behavior through that agent on Windows hosts. That path does not need SPICE guest tools or an extra workflow flag; it starts automatically once the guest agent is reachable.

Incorrect pattern:

```yaml
runtime: vm
resolver: qemu

steps:
  - id: guest-command
    kind: host
    cmd: echo should-not-be-host
```

`qemu` already delegates through the resolver/runtime bridge. Marking the step as `kind: host` conflicts with that contract and Dockpipe will reject it.

When `vm.guest_path` is set, Dockpipe treats the effective workdir as the default host context and maps it into the guest before the guest command runs. Override that with `vm.host_context` when you want to sync a different host directory.

When you need more than one host-to-guest mapping, use `vm.mounts`. Each `host` -> `guest` pair is applied in order before the guest command runs. The older `vm.host_context` + `vm.guest_path` shape remains as sugar for a single default mapping.

For package-specific defaults and mappings, prefer typed `inputs:` over raw `DOCKPIPE_VM_*` workflow vars. That keeps the resolver contract authored as field paths such as `General.ExecMode` or `Advanced.KeepAlive`, while still letting users bind their own globals through `from:`.

This resolver exports its typed model surface itself, so consumer workflows do not need to add a manual `types:` path just to use `inputs:`.

## Guest Tools and host VM page

This resolver contributes the `dockpipe.vm` host page through its package surface
descriptor. The host renders the generic package contract; this package owns VM
inventory, SSH probes, repair and shutdown operations. A running process is shown
separately from an authenticated guest connection and current Guest Tools status.
Each operation rechecks the runtime's public PID and kernel start identity.

The installed `assets/tooling/bin/dockpipe-vm-manager` receives bounded lifecycle
notifications from the VM runtime. Connection records contain key file references,
never key contents or account passwords. The public `dockpipe scope --package
vm.management.v1` contract locates mutable package state. The manifest declares
package-owned compatibility import so ordinary record updates are not mistaken
for immutable legacy-import evidence. No legacy VM state is adopted implicitly.

Build the native manager with the catalog's pinned CLI and locked Go SDK inputs
through the VM family's source hook. The catalog producer runs source hooks after
ordinary resolver/workflow compilation so the final resolver artifact includes
its native tools. Set `DOCKPIPE_VM_HOST_QT_PREFIX` explicitly to the supported Qt
kit when building the clipboard bridge closure; installed packages contain the
result and do not build from sibling checkouts.

Windows installation ISOs and guest disk images are always operator-supplied.
Neither the resolver nor Guest Tools artifacts include operating-system media,
activation material or preinstalled guest disks. The ordinary external QEMU window
remains available for installation and login when using Dockpipe from the CLI.

CLI users can run the same management operations without the launcher:

```sh
dockpipe --workflow vm-manage
dockpipe --workflow vm-manage --env DOCKPIPE_VM_ACTION=repair --env DOCKPIPE_SURFACE_RESOURCE_ID=<run-id>
```

The default action emits a status snapshot. Supported actions are `snapshot`,
`repair`, `open`, `shutdown`, `clipboard-start` and `clipboard-stop`. The QEMU
resolver and the `vm-manage` workflow must both be installed. A background host
clipboard bridge needs an existing desktop session; headless operation still
supports lifecycle and status without a host clipboard.

Launcher-origin display routing uses the generic per-invocation
`DOCKPIPE_SURFACE_LAUNCH_ID`. The VM lifecycle reporter passes it to the package,
which preserves it in the session record and emits `launch_id` with snapshots.
The host routes only an exact outstanding launcher request to its native display;
concurrent terminal launches cannot satisfy that request. Ordinary terminal GUI
launches retain QEMU's standalone window. The selected VM can also be viewed
explicitly in the host VM page or opened from the dashboard dock's Canvas/Tiling
picker.

A qualified host-specific package can launch QEMU's local DBus display into the package's
native Qt display widget. Its surface action uses transient, platform-specific
`launch_environment` references to the installed `dockpipe-qemu-launch` binary.
An operator's explicit `DOCKPIPE_VM_QEMU_BIN` is captured as
`DOCKPIPE_VM_QEMU_REAL_BIN` before that invocation substitutes the shim. The
source producer writes only the built host's Linux, Windows or macOS adapter
binding; each closure must match that host's Qt and compiler ABI. Windows guest
launch workflows remain limited to their declared host platforms. Native display
adapter support alone does not add macOS Windows-guest launch support.

The shim selects the native frontend automatically only for a launcher-origin
request with an installed, digest-verified display factory and an actual QEMU
`dbus` display capability on QEMU 10.2.4 or newer, the qualified Unix.Map
version floor. An authored `DOCKPIPE_VM_DISPLAY` preserves that
explicit choice. `DOCKPIPE_VM_FRONTEND=off` keeps the standalone display;
`DOCKPIPE_VM_FRONTEND=native` requires the qualified backend and reports a clear
failure when it is unavailable. The optional absolute
`DOCKPIPE_VM_NATIVE_QEMU_BIN` selects a separate DBus-capable executable for the
native path. Ordinary terminal launches and QEMU version probes continue using
the stock or explicitly selected real QEMU executable.

Each native display has its own session bus in a new owner-only runtime
directory. A package supervisor keeps it alive across QEMU daemonization and
removes its socket and descriptor when the exact QEMU process start identity
ends. The private `display-<run-id>/display.json` descriptor stays inside the
public SDK-resolved package state scope with owner-only permissions. Public
snapshots expose only the resource identity and state reference. The widget
validates the descriptor, process owner and kernel start token before connecting.
Existing visible QEMU sessions retain their native-window display metadata.


## Pointer and resolution settings

Native views default to a standard USB HID tablet for absolute pointer input.
`Compute.PointerMode` / `DOCKPIPE_VM_POINTER_MODE` accepts `auto`, `absolute` or
`relative`. Auto/absolute retains an existing tablet or adds a dedicated virtual
USB controller and tablet. Relative retains relative hardware and uses click to
capture with **Ctrl+Alt+G** to release; focus loss and view closure release input.
No GPU passthrough is required. Wayland relative capture is unavailable; use an
absolute tablet there. Native cursor shapes follow QEMU's guest-cursor messages,
and the host cursor is restored outside the active guest viewport.

Choose **Display resolution** from a running Windows resource's VM menu to run
the public `vm-resolution` workflow. Its typed width/height and optional CLI run
ID are also available to YAML callers. Guest Tools applies only resolutions
advertised by the Windows display driver, in the logged-in interactive session,
and saves the selected mode. Guest Tools itself lists the actual supported modes.
An unsupported mode fails without changing geometry. A Windows Basic Display
Adapter can expose only its firmware boot mode; changing live resolution then
requires a compatible display driver. The controls do not silently install a
driver or change virtual GPU hardware.

## Boot display preference

`Compute.BootDisplayWidth` and `Compute.BootDisplayHeight` are optional paired
settings for the next VM start. They default to empty, preserving QEMU's existing
hardware defaults. For example, workflow inputs may supply:

```yaml
inputs:
  Compute.BootDisplayWidth: "1920"
  Compute.BootDisplayHeight: "1080"
```

The generic runner accepts width 640–4096, height 480–2160 and at most 4,194,304
pixels to fit the standard VGA 16 MiB framebuffer. Both values must be plain
decimal integers. These settings add only standard-VGA `xres`/`yres` EDID
preferences; they do not replace a video device or configure GPU passthrough.
Other video adapters may ignore them. A new VM start is required.

This preference does not add live Windows modes. The installed guest driver
still determines what `EnumDisplaySettings` reports. If Windows Basic Display
exposes only one mode, the live resolution selector must show only that mode;
boot preferences and later driver qualification remain separate operations.

### Windows display driver follow-up

Boot EDID preferences do not add modes to a guest driver that exposes only one
resolution. The upstream virtio-win project now has a
[standard VGA KMDOD driver](https://github.com/virtio-win/kvm-guest-drivers-windows/tree/master/stdvga)
for the existing QEMU standard VGA PCI device. Its documentation describes direct
Bochs VBE mode setting on Windows 10/11, including UEFI guests where Microsoft
Basic Display may retain the firmware mode. This is a possible later driver path;
it has not been installed or qualified by this package.

The official [virtio-win distribution documentation](https://github.com/virtio-win/virtio-win-pkg-scripts/blob/master/README.md)
describes Microsoft attestation-signed Windows 10+ builds. Attestation signing is
not WHQL qualification. Before proposing installation, select an exact release,
verify its hash and the driver's catalog signature and trust chain on Windows,
and confirm the standard VGA hardware match. No exact driver artifact or its
signature has been verified here. Do not disable driver-signature enforcement or
use a locally test-signed build as a substitute.
