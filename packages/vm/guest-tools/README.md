# Windows Guest Tools

This package owns the standalone Qt Widgets application that installs and monitors
Windows guest services. The runtime owns OpenSSH, guest-agent provisioning and the
SYSTEM startup repair task. Their implementation is not copied into this package.
The host's native VM page is a separate package participant, built against the
matching exact LauncherSDK; this guest application does not link that host SDK.

The UI normally runs without administrator rights. Install or repair requests
Windows elevation for the same executable and retains the original interactive
username. The package installer verifies every application file, installs an
immutable bundle beneath `Program Files/Dockpipe/GuestTools/versions`, registers
a per-user limited logon UI task, and invokes the runtime-owned helper with
`-RegisterOnly`. If the existing durable configuration and task exist it starts
that task instead. SYSTEM provisioning continues when the UI closes. No password
or local-user creation changes are requested.

The UI reads the bounded, nonsecret runtime contract at
`%ProgramData%/Dockpipe/GuestToolsStatus/status.json`. It shows actual provisioning
phases, failure, SSH/agent health and startup repair registration. Its `--startup`
mode keeps a hidden per-user tray service when the runtime reports readiness for
the current boot. Closing the window hides it without stopping clipboard IPC or
SYSTEM repair. The tray provides explicit Show and Exit actions.
`--check-status` prints a compact readiness result for native acceptance. Users
have read-only access; the runtime protects status writes for SYSTEM/admins.

## Reproducible native build

`tooling/toolchain.lock.json` pins six official Qt/MinGW/CMake/Ninja archives by
exact URL, byte count and SHA-256. `tooling/extractor-requirements.lock.txt` pins
py7zr 1.0.0 and every Windows Python 3.13 dependency by version and wheel digest.
Use a dedicated extractor environment and `pip install --require-hashes` with
that lock. Verify the archive hashes before extraction, then run:

```powershell
python tooling/install_toolchain.py --archives C:/build/qt-archives --output C:/build/Qt
$env:DOCKPIPE_GUEST_TOOLS_QT = 'C:/build/Qt'
$env:DOCKPIPE_GUEST_TOOLS_OUTPUT = 'C:/build/guest-tools-qualification'
# From the catalog root, using its checksum-locked installed Dockpipe 0.6.25:
& $env:DOCKPIPE_BIN --workflow-file packages/vm/guest-tools/workflows/build/config.yml --
```

The producer requires native Windows, exact Qt 6.10.3 and MinGW 13.1.0. It builds
the application, runs the status-contract tests and deploys its private Qt/runtime
closure with `windeployqt`. The resulting bundle includes an exact file inventory,
installer and upstream license texts. Toolchain installation is task-local and
does not install Visual Studio or modify the machine-wide PATH.

Use the complete bundle with the original runtime-owned helper/agent bootstrap
media. The application discovers that helper beside itself, one directory above,
or in the durable runtime provisioning directory. Bundle inventory files contain
hashes, never credentials. GUI rendering, elevation, successful deployment and
independent boot repair require native Windows evidence beyond compilation.

## Interactive clipboard boundary

The limited interactive process owns Windows clipboard access. It listens only on
`dockpipe-guest-clipboard-v1-<Windows user SID>` with Qt's `UserAccessOption`.
Requests are newline JSON, at most 1,572,864 wire bytes and one per connection:
`{schema: 1, operation: "read" | "write" | "show", text_base64?: "..."}`.
Responses contain `schema: 1`, `success`, optional `text_base64`, and a fixed safe
`error` on failure. Write text must be canonical base64 decoding to valid UTF-8 without embedded NUL (the Windows text terminator),
at most 1,048,576 bytes. Image/file formats are not converted to text; a nontext
read returns `unsupported-text`. No clipboard contents are logged.

The local server accepts at most eight clients with five-second request/reply
lifetimes. It does not itself start host synchronization. The host's explicit
clipboard start action owns that session and its teardown. An existing process
receives `show` when another ordinary app launch requests its window; elevated
repair windows remain separate from the limited process owning clipboard access.

## Delivery and first installation

The producer writes `dockpipe-guest-tools-1.0.0-windows-amd64.zip` and
`guest-tools-release.json` beside `bundle/`. The schema-1 descriptor binds the
archive name, exact byte count and SHA-256, inventory SHA-256, package source
inventory digest, product/version/platform/Qt version and relative entrypoints.
It contains no publication URL; a local build does not establish remote availability.
The package producer must verify the descriptor and inventory before placing the
complete bundle in its declared resolver assets, rather than compiling sibling
runtime source or copying a partial application.

Runtime media can then deliver the verified bundle as `GuestTools/`, with the
runtime-owned `provision-windows-ssh.ps1` and matching guest agent at the media
parent. Open `GuestTools/dockpipe-guest-tools.exe` and choose Install or repair;
Windows requests UAC approval. This one-time explicit installation persists the
application and per-user logon UI task, plus the runtime SYSTEM startup repair.
Subsequent boot repair is independent of login or the application window.
Windows AutoPlay policy may offer the entrypoint, but a fresh OS is not assumed
to permit unattended AutoRun. The source-only repository does not provide a
published binary until its separate artifact publication gate completes.

Write requests may add `expected_sha256`, either the exact lowercase SHA-256 of
the current UTF-8 text or `unsupported-text`. The interactive Windows process
holds `OpenClipboard` while checking that identity and writing the replacement.
A changed identity returns `conflict` without mutation. Empty clipboard state can
be read as empty text; images and files remain unsupported. The host bridge uses
conditional writes and restores proof fixtures only if the expected identity
still matches, preserving intervening user copies. No clipboard hashes are logged.

RPC reads and conditional writes share one bounded native `CF_UNICODETEXT` reader
in the interactive process. They preserve the same exact UTF-8 representation,
including Windows CRLF and LF differences, without Qt MIME newline conversion.
