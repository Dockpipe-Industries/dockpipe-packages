# Native local QEMU display

This package owns a native Qt frontend for headless QEMU's official local D-Bus
display API. `DisplayWidget` renders a bounded `FrameStore`; it has no dependency
on QEMU, GLib, a host window handle or the VM lifecycle. `QemuDisplayTransport`
owns the separate GIO worker and Unix listener connection. The installed
LauncherSDK or minimal DisplaySDK adapter implements `com.dockpipe.launcher.DisplayPackage/1.0`.
Closing or transferring a host view releases input, cancels/joins its worker and
disconnects its listener. It never stops or restarts QEMU.

## Qualified path

| Capability | Linux amd64 / X11 | Windows amd64 / MSVC | macOS arm64 / Cocoa |
| --- | --- | --- | --- |
| Qt 6.10.3 software QWidget rendering | Native visible firmware proof | Native visible firmware proof, limited user | Native visible firmware proof |
| Local shared framebuffer | Unix.Map, resize | Win32.Map, resize | Unix.Map, resize |
| Raw local framebuffer | Firmware resize and actual Windows guest driver proof | Parser/unit coverage; native raw-only proof pending | Parser/unit coverage; native raw-only proof pending |
| Dedicated QMP absolute input | Task candidate firmware and exact Windows protocol probe; final installed guest widget pending | Not yet run | Not yet run |
| Keyboard and pointer protocol | Native acknowledgements; physical key map unit tests | Native acknowledgements; physical key map unit tests | Native acknowledgements; physical key map unit tests |
| Installed factory handoff/close | Generic host wrapper and shared lease proof | Minimal installed Display SDK plugin proof | Minimal installed Display SDK plugin proof |
| Private broker/runtime artifact | Installed Go shim and compiled archive proof | Native isolated module/broker artifact proof | Native relocated artifact and broker proof |
| Full installed desktop launcher route | Actual Windows guest VM tab, Canvas and Tiling proof | Not run | Task-private installed application: VM page, Canvas, Tiling, input, close/reopen |
| DMA-BUF, GPU texture import, zero-copy | Unimplemented | Unimplemented | Unimplemented |
| Audio, clipboard, multitouch, multi-monitor | Unimplemented in this frontend | Unimplemented in this frontend | Unimplemented in this frontend |

Native Windows and macOS proofs use disposable x86 firmware under TCG, without
an OS disk or network. They qualify these adapter/module paths, not a Windows
or macOS desktop product release. The Windows VM workflow currently advertises
Windows and Debian hosts; these display proofs do not add a macOS guest-launch
workflow contract.

The Linux-qualified backend uses the local QEMU API with
`-display dbus,...,gl=off`. `ScanoutMap` supplies authoritative dimensions,
stride, format, offset and a shared-memory FD. `UpdateMap` identifies dirty
rectangles. FD bytes are copied with bounded `pread` into Qt-owned images.
There is no host-window reparenting, remote display protocol, direct mmap,
GPU import or zero-copy claim.

Raw `Scanout`/`Update` is also a supported local software delivery path. The
pinned QEMU backend can send a 640x480 placeholder `Scanout`, followed by
720-pixel-wide firmware `Update` rows without an intervening raw `Scanout`.
Updates enter a bounded 32 MiB/128-rectangle batch while an asynchronous
`Console` property query obtains authoritative Width/Height. Only those
properties can resize the canvas; damage extents cannot grow it. A generation
check discards superseded geometry results when a new Scanout/ScanoutMap arrives.
At most four geometry queries may remain outstanding. No callback blocks QEMU's
synchronous Map negotiation. Native raw-only firmware resize/input proof passes.

Factory readiness requires an authenticated connection and a validated frame,
whether shared or raw. A five-second initial frame deadline reports failure;
the generic qualification harness observes each view for more than six seconds
and requires its `displayReady` property. `displayTransportKind` reports the
current delivery path for proof metadata. A raw Windows guest-driver scanout
is not misclassified as missing shared-memory capability.

## Pointer behavior

Absolute input uses QEMU console pixels, with inclusive pixel endpoints across the
rendered image. Fractional host coordinates, letterboxing and scaling share the
same mapping. A click updates guest position before its button event; a drag
release outside the image clamps to its edge so the guest button is released.

QEMU cursor shapes are copied as bounded ARGB images (at most 256 by 256) with
validated hot spots. Absolute hardware cursors use a cached native Qt cursor
scaled with the image. The software-cursor path follows QEMU's active-view policy:
the host pointer is hidden inside a valid absolute guest image or a relative
capture, and restored outside, on deactivation and during view handoff. This
policy accompanies coordinate and capture corrections; it is not a replacement
for them. Relative hardware cursor overlays require QEMU position feedback.

Relative mode requires a click to capture. Motion is converted from logical
widget coordinates to guest pixels, preserving fractional movement. Recentering
allows continued travel beyond the host window edge without sending the warp as
reverse guest movement. **Ctrl+Alt+G**, focus loss, hiding the view or deactivation
releases capture and held input. The tooltip explains capture and release.
Wayland relative warping is unavailable; use an absolute tablet there.

The `display-widget-input` tests exercise the four console-pixel corners, scaled
and fractional input, position-before-click, letterbox rejection, drag release,
relative scaling/recentering and focus/capture cleanup. `display-pointer-state`
checks cursor bounds, ARGB ownership and concurrent snapshots. These are Qt
fixtures; exact guest OS corner landing requires a separate interactive guest
position measurement through the installed module.

## Platform adapters

Windows builds use GLib/GIO >=2.72 and an explicit native prefix. A protected
AF_UNIX pair is created using GSocket, then `WSADuplicateSocketW` serializes the
QEMU-targeted endpoint for `RegisterListener(ay)`. `Win32.Map` receives a transferred
mapping handle. The frontend owns it, maps an explicitly bounded read-only view,
checks committed mapped extent, and copies rows using `ReadProcessMemory` so
invalidated views fail instead of causing unchecked access. Replacement and
cancellation unmap and close the handle. Metadata admission checks the current
user SID and rejects wider ACL grants/reparse points. Process identity uses the
same SID and exact decimal FILETIME creation token. Native mapping/process tests
have run, as has the minimal installed-module shared-memory handoff proof. The
complete Windows desktop host route remains a separate qualification.

macOS uses the Unix FD protocol and bounded pread from an owned unlinked regular
file, including QEMU's non-memfd allocation fallback. Linux retains its additional
tmpfs admission check. Darwin identity uses libproc BSD start seconds:microseconds
and current UID. Supervisors must publish canonical private paths (`/private/tmp`
rather than the `/tmp` alias). Cocoa mapping uses Qt nativeVirtualKey; Windows
mapping uses Qt's extended nativeScanCode. Native macOS firmware, input and
minimal installed-module proofs pass with the patched private broker. The full
task-private installed macOS application also passes its real VM page, Canvas
and Tiling pickers, native Shift input, single-view handoff, close and reopen.
This uses the public Go shim and native process snapshot with foreground managed
QEMU. The earlier daemonized fixture is not qualified. It does not add a macOS
guest-launch workflow.

## Private endpoint contract

The host passes only this opaque public reference:

```json
{"schema":1,"resource_id":"run-id","state_ref":"/absolute/package-state/display-run-id"}
```

The exact owned mode-0700 real directory contains `display.json`, a regular
non-symlink, single-link, owned mode-0600 file of at most 4096 bytes:

```json
{"schema":1,"resource_id":"run-id","backend":"qemu-dbus","bus_address":"unix:path=/private/runtime/bus","console_id":0,"qemu_pid":1234,"qemu_start_token":"123456"}
```

The package supervisor owns publication and removal. The frontend checks all
seven fields and the current UID/PID/kernel start token (`/proc/PID/stat` field
22), then admits only an owned Unix socket in an owned private directory.
It resolves `org.qemu` to its unique bus owner and requires
`GetConnectionUnixProcessID` to match that expected QEMU process before
registration or input. All subsequent calls target that unique owner. The
worker keeps checking kernel identity and disconnects when it expires. It
never discovers arbitrary system/session-bus QEMU instances.

## Bounds and failure behavior

- Dimensions are nonzero and at most 4096; stride is aligned, at least width*4,
  and stride*height is at most 32 MiB. Only native-endian PIXMAN_X8R8G8B8 and
  PIXMAN_A8R8G8B8 are accepted; display output is opaque, top row first.
- Unix shared FDs must refer to current-user-owned, unlinked regular files;
  Linux additionally requires tmpfs.
  The offset plus payload must fit the actual file and the 256 MiB source
  bound. The frontend owns its received duplicate and closes it on replacement,
  disable, failure or cancellation. Every dirty rectangle stays inside the
  current scanout. Short reads/truncation fail safely without mmap SIGBUS.
- The mailbox retains the latest image, without queued image events. Qt takes
  shared snapshots and detaches on writes. A payload and Qt copies can coexist;
  32 MiB is a per-frame bound, not a whole-process limit. GDBus has its own
  upstream message-size bounds and can allocate a message before our validation.
  The private, PID-checked QEMU producer is part of this trust boundary.
- At most 128 input operations queue. Absolute motion coalesces. Overflow fails
  the connection; releases are not silently dropped. Clean deactivation releases
  tracked keys/buttons, with a 250 ms drain allowance before cancellation.
  A disconnected peer cannot guarantee guest input release.
- Authentication and synchronous operations use a cancellable; calls have finite
  timeouts, callback batches are bounded, and teardown joins the worker. Failed
  frames stop the connection and leave a visible disconnected status rather
  than presenting a stale frame as current.
- No clipboard, guest credentials, activation controls or guest window captions
  are used. Bounded diagnostics contain method names and numeric frame metadata,
  never pixel contents. Proof screenshots contain only the disposable firmware.

## Private Darwin broker credentials

The observed Homebrew D-Bus 1.16.2 broker provided peer UID but no PID, so the
frontend correctly rejected it. `dependencies/dbus-macos-source.lock.json` pins
the official source and package-authored patch. The patch adds Darwin kernel
`LOCAL_PEERPID` acquisition and the missing CMake `getpeereid` feature detection.
Native proof verifies the service PID, UID and process start token before display
admission. This private dependency preserves upstream licenses and patch identity;
it never replaces a system service or relaxes admission to UID alone.

## Native display producer

`build.py` produces the package-owned display module, its private broker and their
bounded native runtime and license inventories. Select an explicit installed
Launcher SDK or Display SDK, the exact matching Qt kit, and a hash-verified broker
input. These are producer inputs, not sibling source fallbacks.

Required inputs are `DOCKPIPE_VM_DISPLAY_SDK_PREFIX`,
`DOCKPIPE_VM_HOST_QT_PREFIX`, `DOCKPIPE_VM_DBUS_DAEMON_BIN` and
`DOCKPIPE_VM_DBUS_PROVENANCE`. `DOCKPIPE_DISPLAY_SDK=display` explicitly selects
the Widgets-only Display SDK; the default remains the full Launcher SDK.
Windows additionally selects `DOCKPIPE_GIO_PREFIX` and an absolute native
`DOCKPIPE_VM_DISPLAY_INSPECTOR` (`dumpbin` or `objdump`). The matching host supplies
Qt and the documented MSVC 14 runtime family; the producer does not harvest
System32 files. The private broker ships under `tooling/bin` beside this module's
runtime closure. Broker provenance records its exact input hash, primary source
identity and bounded hash-verified license files.

Run the producer through the owning Dockpipe workflow with a fresh absolute
`--output` directory. A normal invocation configures, compiles, tests and packages.
`--prepared-build /absolute/build` explicitly reuses a previously qualified
release build: the producer requires matching material CMake/C++/header/template
sources, selected installed SDK/Qt/native-prefix paths and exact generated
Qt/compiler/platform ABI metadata. The original material source tree must still
be available for this comparison. A mismatch fails rather than rebuilding from
another tree. Packaging-only Python changes do not require recompilation.

Prepared builds still run CTest unless `--skip-tests` is explicit. When native
permission or socket tests require a separate reviewed user/session, preserve the
external test receipt and report `tests_run: false` honestly in the producer
inventory. Compilation, unit tests, actual framebuffer/input proof, installed
plugin proof and complete desktop-host integration remain separate claims.

When a native bottle omits license files, the producer uses only an exact
name/version match in `dependencies/runtime-licenses.lock.json`. The GLib
2.90.1 entry preserves all eleven license texts extracted from its verified
upstream source archive. A different version or changed text fails admission;
there is no generic license-text substitute or network lookup during production.

The module emits `display-factory.json`, source/runtime/license inventories and
`inventory.json`. Linux uses `$ORIGIN/lib`; macOS uses `@loader_path/lib` and
relocates/signs copied native dylibs. The exact matching host supplies Qt. The
captured Linux distribution/runtime closure is not a universal ABI guarantee.

`tooling/firmware_proof.py` runs through a Dockpipe host workflow. It creates a
private EXTERNAL-only bus configuration without system includes, starts only
owned no-disk/no-network QEMU children, and records bounded shutdown and directory
cleanup. Its screenshots and receipts belong in task artifacts. The optional
minimal factory harness requires a real nonuniform canvas at each capture and
qualifies the installed interface rather than the complete desktop host.

API authority: [QEMU D-Bus display](https://www.qemu.org/docs/master/interop/dbus-display.html),
with the signatures checked against QEMU 10.2.4 `ui/dbus-display1.xml` and
`ui/dbus-listener.c`. Native host proof receipts identify their actual backend
builds separately; a newer native backend is not relabelled as the pinned source.

## Installed SDK native-library scope

The minimal factory harness consumes the core-owned installed
`LauncherNativeLibraryScope.h` after validating the module ABI, Qt and compiler
metadata. On Windows the helper uses a native-separator absolute path with
`LoadLibraryExW` restricted to the DLL directory and default safe locations. It
holds the dependency scope through widget close and loader teardown, without
changing PATH, the current directory or process-wide DLL search policy. The
helper is a no-op on other hosts. Its implementation remains in the host SDK;
this package includes the installed header and does not copy it into its source.

The Windows isolated artifact proof uses the matching Qt host runtime and the
module's contained native dependencies, rather than an ambient GIO directory.
This qualifies the installed module and broker path. The separate full installed
macOS development-candidate proof is recorded under
`macos-host-validation/foreground-live-proof-result-37/extracted`: all five real
MainWindow views contain firmware pixels; native input is acknowledged; closing
a view leaves QEMU alive; final owned QEMU, supervisor and broker cleanup removes
the private state. The protected `/Applications/DockPipe.app` is unchanged. This
is not a signed release or publication claim. The full installed Windows route
remains unrun at this checkpoint.

## Absolute pointer endpoints

New native launches reserve a separate owner-private QMP input monitor beside
this resource's private display broker. The ordinary management monitor remains
available to the VM controller. Private `display.json` schema 2 adds
`input_address`; the public DisplaySDK and resource options remain unchanged.
The module admits the exact QEMU process creation token and native Unix socket
peer PID before using this channel. Windows uses AF_UNIX, not a named pipe or TCP.
Only `qmp_capabilities` and typed `input-send-event` absolute-axis pairs are sent.
No generic monitor command interface is exposed to the host UI.

The existing display worker serializes motion with D-Bus button/key events. It
waits for each fixed request's exact acknowledgement, bounds each exchange to two
seconds and 32 asynchronous events, and rejects messages of 64 KiB or more. A
failed exchange closes the connection; a later move cannot consume an old reply.
Closing the display cancels and joins its worker without stopping the VM.

Pixels from zero through `dimension - 1` map to QMP's inclusive 0..32767 range.
On the qualified Windows tablet, the all-zero pair did not move the cursor;
normalized `(1,1)` reaches guest pixel `(0,0)` exactly. This special case changes
neither the requested pixel nor the acceptance tolerance. Frames are bounded to
4096 pixels per axis, where normalized one still rounds to pixel zero. Queued
positions carry their frame size. A stale size suppresses the motion and blocks a
subsequent button press until a fresh position succeeds; releases remain allowed.

Schema 1 endpoints from existing sessions keep their original D-Bus input path
and its known QEMU coordinate-scaling limitation. They do not silently borrow the
management QMP socket. A clean native relaunch is required to create the dedicated
schema 2 channel. Multiconsole QMP input is not qualified; schema 2 admits console
zero only. Native Windows/macOS channel behavior and exact Windows widget-corner
acceptance require their own receipts in addition to Linux protocol tests.
