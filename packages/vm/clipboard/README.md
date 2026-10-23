# VM clipboard bridge

This package-owned Linux Qt process shares bounded UTF-8 text with the logged-in
Windows Guest Tools application. Enable it explicitly with the VM resource's
clipboard start action. Starting establishes both clipboard baselines; subsequent
copies synchronize. It never reads or changes Windows activation.

The host bridge launches the adjacent VM manager's private `clipboard-exchange`
operation. Requests and responses travel on private stdin/stdout, never command
arguments, environment variables, workflow events, or logs. The manager verifies
the VM's public process PID and kernel start token before and after each SSH
exchange and requires its previously established SSH host identity. The fixed
PowerShell transport connects to the interactive user's SID-scoped named pipe;
it does not use the SYSTEM guest agent's Session 0 clipboard.

Both endpoints accept at most 1 MiB of valid UTF-8 text and 1.5 MiB of newline JSON.
The optional `expected_sha256` write condition is checked inside the Windows
clipboard lock, so a concurrent user copy is preserved. Simultaneous changes on
both sides preserve both values and wait for another copy. Unsupported formats
are ignored. Disconnects or lost VM identity pause the bridge until an explicit
start action reconnects. Only the current user's local control socket is exposed.

Build with an explicitly selected Qt 6.10.3 Linux kit:

```sh
DOCKPIPE_VM_HOST_QT_PREFIX=/absolute/qt/6.10.3/gcc_64 \
  python3 packages/vm/clipboard/build.py --output /absolute/staging/assets/tooling
```

The output includes `bin/dockpipe-vm-clipboard`, relative `qt.conf`, Qt/ICU library
closure, XCB/Wayland platform plugins and `clipboard-artifact.json` with exact
file sizes/hashes and kit version. Linux system libraries remain OS dependencies.
The VM package hook installs the separately pinned-SDK-built Go manager alongside
it. No Qt installation or source checkout is searched at runtime.

The bridge supports `--start`, `--stop`, and `--status`, each with `--resource`
and an absolute `--manager` executable. The required
`DOCKPIPE_VM_CLIPBOARD_SCOPE` is the SDK-resolved absolute package-state directory;
worker identity uses its canonical path and the resource ID, independent of which
package cache or extracted manager copy invokes it. Start detaches its worker and closes
workflow stdio. Stop closes the worker; the normal desktop clipboard manager
controls persistence after an owning application closes.

`--verify` requires the real desktop and the running interactive Windows app. It
keeps original guest text and bounded original host MIME data only in memory,
uses generated Unicode roundtrip markers,
pauses synchronization before restoring, and conditionally restores only markers
that have not been replaced by another user copy. The long-lived bridge retains
host clipboard ownership after restoration. Its output contains only success,
direction, and restoration booleans; it refuses unsupported initial formats.

For a clipboard-free lifecycle check, set `DOCKPIPE_VM_CLIPBOARD_BIN` to the built
binary and run `python3 packages/vm/clipboard/tests/control_test.py`. The fixture
forces Qt's offscreen platform and does not access the desktop clipboard.
