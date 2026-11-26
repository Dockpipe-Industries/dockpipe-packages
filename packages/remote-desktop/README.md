# Dockpipe native remote desktop

`remote-desktop` 0.1.0 is an unpublished native provider candidate. Resolver
`dockpipe.desktop.native` owns capture, physical input mapping, H264 media, the
native display module, and its private authenticated channel helper. Dockpipe
core owns the generic authorized control session and bounded binary relay; the
host assembles the page through `com.dockpipe.launcher.DisplayPackage/1.0`.
No package-specific capture or codec implementation belongs in core.
The resolver exposes its contained Linux module through the generic `surface`
descriptor; its empty-action snapshot reports no resources and starts no session.
Darwin sender artifacts omit the viewer declaration.

The viewer accepts the generic host launch envelope `dockpipe.remote.desktop-display/v1`.
The prior `dockpipe.desktop.native/v1` envelope remains accepted for existing hosts.
The package validates its own provider identity and supported capture, display and
decoder preferences; the host does not select a native protocol for other resolvers.

The first supported build roles are **Linux amd64 viewer** (X11 physical keyboard mapping) with the exact Qt
6.10.3 kit and **macOS arm64 sender** on macOS 13 or later. Compilation, fixture
success, native OS permission onboarding, actual capture/input, installed module
loading, distribution closure and paired end-to-end qualification are separate
claims. Windows, Linux capture, Darwin viewing, audio, file transfer,
IME/text injection, multiple monitors and persistent unattended capture are not
implemented by this candidate. The manifest's minimum Dockpipe version is a
package-format baseline; the native desktop control capability additionally
requires a matching qualified host build.

## Authorization and connectivity

Use the normal broker browser setup and an existing paired worker. Each session
requires a new target-bound native desktop grant; view and input authority remain
separate. Native macOS capture and event posting require the final signed worker
application's OS permissions. A temporary CLI permission result does not grant a
later signed application's TCC identity. Opening a view never silently enables
native permissions or widens a view-only grant. The initial WSS route requires no
pasted Cloudflare API token, TURN key, WARP enrollment or private CIDR route.

A package-owned Go standard-library TLS endpoint wraps the native channel through
the authenticated WSS binary relay. Single-use identity, exact local/peer
fingerprints and canonical session target/generation are checked before media or
input. Broker TLS termination alone is not peer encryption; the package's inner
TLS channel supplies that distinct boundary. Do not place bearer credentials in
URLs, package manifests or display options. Session identities, private configs
and authority channels belong to owner-private runtime state, not package assets.

The existing public broker hostname and Cloudflare Tunnel remain operator-owned.
There is no tunnel/account/service mutation in package build or tests. Cloudflare
supports WebSocket routing, including RDP, but connector/edge restarts can drop
connections. Account plan/entitlement and the terms classification of sustained
interactive native desktop traffic are not established by an upgrade or random
payload probe. Public-hostname video/large-file restrictions must be reviewed in
context; this package makes no free or unlimited bandwidth claim. Encryption or
chunking does not establish a terms exemption. TURN remains a future fallback;
its setup must use supported browser authorization rather than manual token
paste. No TURN or libdatachannel dependency ships in this WSS slice. The Go TLS
channel and Darwin sender require no OpenSSL. The selected Linux FFmpeg build
uses Qt backend stubs; its host Crypto/VAAPI symbol dependencies require separate
explicit linkage and actual module-load qualification.

## Resource and lifecycle contracts

The macOS worker completes AppKit launch with the accessory activation policy:
it stays in the background without a Dock icon, application menu or window.
Explicit local permission onboarding retains its separate foreground mode in
the same signed Dockpipe app. Normal app launches keep the regular UI.

- The canonical session binds ID, epoch, worker fingerprint and generation.
  Lease renewal is session-specific, cannot broaden input authority and cannot
  resurrect a stopped or expired provider. A failed attach consumes its private
  single-use identity; reconnect requires fresh authorization.
- The sender fixes the primary display at admission. Geometry/display changes
  terminate capture. Frames retain aspect ratio, have even dimensions no greater
  than the frozen profile limit (up to 3840 by 2160), use a frozen negotiated quality/FPS profile, and use hardware H264 constrained
  baseline, maximum Level 4.0. Hardware encoding failure has no silent software
  fallback. SCStream queue depth is three; at most two encoder frames are in
  flight. These are per-component bounds, not a whole-process memory guarantee.
- A video envelope is at most 2 MiB including its 32-byte header. The viewer
  checks sequence, AnnexB framing and authoritative geometry, and caps decoder
  output at its frozen profile bounds (up to 3840 by 2160) with two decoder threads. Backpressure must remain
  bounded; stale video must not become an unbounded work queue.
- Stdin, stdout and the one-shot readiness descriptor remain pipes. Only the
  inherited lease-control descriptor additionally accepts a connected Unix stream
  socket, as used by the Linux Qt socketpair. The trusted parent supplies the
  descriptor; the helper never discovers or connects a named socket. TCP,
  datagrams, unconnected sockets and regular files are rejected for this lane.
- The TLS record bridge bounds chunks at 64 KiB and control JSON at 16 KiB.
  Handshake, heartbeat and lease deadlines remain owned by the channel, rather
  than disabling ordinary broker HTTP timeouts globally. The relay is opaque
  transport, not an authority source for native input.
- Physical USB HID page 07 events use an exact-next input sequence. Coordinates
  must be finite; only the declared keys, three buttons and pixel scrolling are
  supported. Pending input is bounded at 128. Disconnect/expiry releases only
  this session's held keys/buttons. The viewer filters unsupported physical keys; invalid wire events fail explicitly.
- Closing/deactivating a display releases owned input and cancels/joins its
  workers. Native stop has a five-second watchdog. A hang, revoked posting
  permission or unverifiable child cleanup reports unknown cleanup and prevents
  provider reuse; a supervisor must terminate/reap an owned hung process before
  replacement. The worker services the main Foundation run loop while waiting for
  its own stop completion, concurrently stops/reaps the relay child, and never
  upgrades an earlier unknown result from a late callback. A closed report must
  receive the exact original-reference Forget before the bridge is retired.
  No fabricated success, automatic desktop restart or OS permission
  bypass is permitted. No pixels, raw input, titles, clipboard or credentials
  are journaled. Diagnostic receipts contain bounded identity/outcome metadata.

## Explicit producer inputs

`dependencies/inputs.lock.json` pins immutable published DisplaySDK 1.0.0
artifacts for Linux amd64 and Darwin arm64, their release/file manifests and
DisplayPackage/1.0 header identity; Qt 6.10.3; FFmpeg 7.1.1 headers and the selected
Linux native runtime hashes; nlohmann JSON 3.12.0; and Go 1.26.10. No sibling core,
package, Qt or compiler source is an implicit dependency. Downloaded third-party
implementations are task/build inputs, not authored files copied into this tree.
The standard-library-only channel module uses no published Dockpipe Go SDK.

Prepare an absolute input directory containing the locked archives and metadata:

```text
INPUTS/linux-amd64/{DockPipeDisplaySDK-1.0.0-linux-amd64.tar.gz,sdk-files.json,sdk-release.json}
INPUTS/darwin-arm64/{DockPipeDisplaySDK-1.0.0-darwin-arm64.tar.gz,sdk-files.json,sdk-release.json}
INPUTS/ffmpeg-7.1.1.tar.xz
INPUTS/json-55f93686c01528224f448c19128836e7df245f72.tar.gz
```

For a Linux viewer build, also provide the archives and Qt build recipes listed
under `ffmpeg.linux-amd64_distribution.sources` in the dependency lock. The
producer verifies and includes them under `assets/media-sources`. The private
runtime load test records the loaded FFmpeg version and build configuration.

The Qt test platform follows the producer's `QT_QPA_PLATFORM` (default offscreen).
Native viewer control tests require X11; offscreen runs skip that control proof.
The physical-key mapping fixtures are portable by default. Explicit read-only
native X11 qualification additionally sets `DOCKPIPE_TEST_INPUT_NATIVE_X11=1`
and runs its target with the matching host DISPLAY and `QT_QPA_PLATFORM=xcb`.
Offscreen tests do not qualify an actual host key-name map.

The producer never downloads dependencies. Linux production also runs actual
SDK factory admission, repeated lease IPC, original synthetic H264 decoding and
viewer input-mapping fixtures. A fresh process resolves the relocated output with
RTLD_NOW and verifies all eight private media library origins, so an SDK copy
cannot hide a missing bundled dependency. Archives, internal payload inventories
and runtime bytes are verified before use, with bounded extraction and rejection
of links, duplicate files and escaping paths. Linux explicitly selects the
matching Qt kit; native builds use the host compiler, CMake and Apple SDK as
recorded in the output receipt. Cross compilation is not a native-run receipt.
The Darwin worker is a separate application bundle; a signing identity is
explicit. Ad-hoc signing qualifies neither distribution nor native OS onboarding.

Run the repository workflows with the released CLI pinned by the catalog's
`dockpipe.config.json`:

```bash
export DOCKPIPE_BIN=/absolute/released/dockpipe
export DOCKPIPE_DESKTOP_DEPENDENCY_INPUTS=/absolute/verified-inputs
export DOCKPIPE_DESKTOP_GO=/absolute/go1.26.10/bin/go
export DOCKPIPE_DESKTOP_QT_PREFIX=/absolute/Qt/6.10.3/gcc_64 # Linux viewer
export DOCKPIPE_DESKTOP_HOST_LIBRARY_PREFIX=/usr/lib/x86_64-linux-gnu # locked Ubuntu amd64 profile
export DOCKPIPE_DESKTOP_OUTPUT=/absolute/fresh-output
"$DOCKPIPE_BIN" --workdir /absolute/catalog --workflow-file /absolute/catalog/packages/remote-desktop/workflows/test/config.yml
"$DOCKPIPE_BIN" --workdir /absolute/catalog --workflow-file /absolute/catalog/packages/remote-desktop/workflows/build/config.yml
"$DOCKPIPE_BIN" package test --workdir /absolute/catalog --only remote-desktop
```

The same Darwin worker executable supports explicit local permission entrypoints
`--permissions screen`, `--permissions input` and `--permissions status`.
The request modes foreground this application and use the native OS permission
flow. They never start a CLI bridge, capture a screen, inject input, install a
service or create a grant. The producer runs only argument-parser fixtures and
does not execute any permission mode. Actual permission onboarding belongs to
the final signed bundle and an explicit operator action.

The package source hook uses generic package-runtime state for disposable output
and compiles a self-contained resolver only after successful native production.
It fails clearly when required sources, SDK artifacts, native tools or signing
selection are missing. It never substitutes an old module or builds a sibling
checkout. Output inventories bind source files, dependencies, library/helper
hashes, native ABI and test/signing status. Building does not install globally,
change package defaults, publish a store, start a provider or change a tunnel.

## Text clipboard

The optional `clipboard_text: true` capture agreement enables plain-text clipboard
sharing in control sessions. Omission preserves the existing disabled protocol.
The request, authenticated echo/confirmation and readiness receipt bind the same
flag. View-only sessions reject it before readiness. Older strict helpers reject
this extension before media/input; they cannot silently enable it.

Sharing requires a connected, active, visible and focused viewer. Watch/resume
establishes change-count baselines without exporting previous remote contents or
uploading the existing local clipboard. A paste chord offers current local text;
subsequent system-clipboard changes while focused may also synchronize. X11
PRIMARY selection is ignored. Native physical keys retain their existing meaning:
Mac copy/paste uses Command (the viewer's Meta/Super key) by default. Optional
`display.swap_control_command: true` swaps left/right Control and Command for
remote physical input; it defaults off, and Alt is unchanged. Physical
Control+Alt+Shift+Escape is recognized before mapping and still releases input.
Default behavior preserves terminal Control+C.

The separate encrypted clipboard record carries at most 64 KiB of valid UTF-8
without NUL, never a truncated value. This unchanged text record does not carry
rich text, images or files; independent image/file opt-ins are described below.
The bound applies to transferred text and package queues; native OS clipboard
materialization is not a whole-process memory guarantee. Clipboard contents stay
in memory and are never written to package logs, journals or history.

Epochs retire paused sessions; exact revisions and origin guards prevent echoes
and stale updates. A Write is acknowledged only after the native pasteboard write
succeeds. The viewer holds at most 128 ordered physical input events behind one
absolute three-second acknowledgment deadline, so delayed or failed clipboard
writes cannot paste old text. It retains one outstanding Write and one coalesced
latest text. Focus loss cancels that barrier before releasing actual held input.

The worker polls changed text at most every 100 ms while armed and rechecks its
running session, control authority and lease immediately before clipboard access.
Clipboard replies and video share one bounded serialized writer. Native builds,
installed-module behavior and actual paired copying require separate qualification;
portable or isolated-display fixtures do not prove live macOS clipboard access.

## PNG and regular-file clipboard (source candidate)

Independent `capture.clipboard_images: true` and `capture.clipboard_files: true`
opt into the unshipped kind5 extension. Both default off and require control;
text sharing remains independent and its kind4 bytes/revisions are unchanged.
Present false/null/nonboolean capability fields are rejected as noncanonical.
All selected bits are bound in the exact capture request, encrypted echo,
confirmation and readiness agreement; old helpers reject them before readiness.
They cannot silently downgrade to text or enable sharing in view-only mode.

An explicit rich paste shortcut waits for confirmed transfer and then completes
that original paste once its physical keys are released. File drops transfer
copies into a private receiving cache and then send Command+V to the active
remote application. They do not select a destination under the pointer or
synthesize a native remote drag. Ordinary clipboard changes never initiate a
paste. One pending gesture is bound to the exact session, selection generation,
focus epoch and modifier policy; new input, focus loss, changed content, expiry
or failure cancels it. The gesture is consumed before its normal authorized HID
batch begins, and a partial batch is never retried. Other input/video stay live;
there is no queue of delayed user input. The viewer distinguishes preparation,
acknowledged byte progress and final verification/publication. Arm and
Pause establish a separate focus epoch even when text sharing is disabled, with
an acknowledged stream boundary before new offers. Transfer cancellation has an
exact-identity terminal acknowledgment, bounded in-flight draining and no silent
reuse after a timeout. Focus/lease loss invalidates uncommitted publication.

PNG is bounded separately to 64 MiB encoded and 16,777,216 decoded pixels. The pixel
budget covers two 3840x2160 screenshots side by side (16,588,800 pixels), or 4096²;
one RGBA8 surface needs 64 MiB at that maximum. Adapters inspect dimensions before
decoding, reject samples deeper than 8 bits, and strip ancillary metadata before
decoding while preserving required pixel/transparency chunks. They budget
native/Qt ownership copies and reject unsupported or multiframe representations.
Already-validated canonical RGBA8 PNGs reuse their sanitized compressed stream
and clean pixels instead of repeatedly decoding/re-encoding. Every new untrusted
PNG still passes structural sanitization and full decoding. A one-operation
exact-byte reuse slot cannot authorize another image or a deferred selection.
The encoded limit is independent: an image within the pixel
limit can still have an encoding that is too large, and is rejected rather than
truncated or silently scaled. Framework/OS clipboard retrieval may itself
materialize an image or metadata; these are component bounds, not a whole-process
memory or OS-internal no-fetch guarantee.

A regular-file batch has at most 128 entries and 1 GiB of declared content. Each
basename is at most 255 valid UTF-8 bytes, with separators, control characters,
`.` and `..` rejected. Names are unique under ASCII case folding; the receiving
adapters independently reject collisions under their Unicode normalization and
case-folding rules before store preparation. Indexed per-item folders mean that
exclusive creation alone cannot detect cross-item name collisions; it instead
protects owned storage against overwrites and path races. The 64 KiB manifest budget covers 128 maximum-length basenames
(32,640 bytes) plus fixed entry metadata. Source paths never cross the wire. Only
explicitly selected regular-file descriptors are read; directories, symlinks,
aliases and promised/provider-generated files are unsupported. No recursive
reads or arbitrary remote destination paths are accepted.

Files stream in 64 KiB chunks with at most four uncredited chunks per direction
(256 KiB), one current file and an incremental SHA256. No whole regular file is
buffered in RAM, and its payload is not read before receiver acceptance. A bounded
PNG snapshot/encoding may be staged before offering it to determine its size and
dimensions, but no asset payload is sent before acceptance. Chunk credits follow
staging writes; intermediate item verification and final native publication are
separate acknowledgments. Final commit requires exact sizes, all hashes and
atomic private-file staging before publishing the native image or local file
URLs. Completed storage is durably premarked as potentially exposed before an
OS write, so failure or a crash cannot authorize deleting possibly published
URLs. Committed still requires verified native publication success. Cocoa clipboard clearing and writing are not one atomic OS transaction:
a failed write can change the receiving clipboard, must withhold Committed, and
must not claim preservation or perform an unbounded backup/rollback.

The 2 GiB private cache budget accounts for active and published batches, including
allocation/metadata charges for empty files. It budgets for an incoming batch
beside a published batch; charged overhead can reduce the available content
capacity below two full 1 GiB batches. Actual free-space headroom is reserved first.
Quota exhaustion rejects new work instead of deleting a live clipboard URL
batch. Published files remain owned by an application-lifetime cache while the
OS clipboard references them; closing a viewer cancels and removes uncommitted
staging without invalidating those published URLs. The bounded private received
cache keeps basenames, sizes and content hashes in its ownership/integrity
metadata. Clipboard contents, names, source paths and hashes are excluded from
diagnostic logs and desktop control-session journals.

The existing ordered inner TLS channel is retained. One bounded serialized
scheduler prioritizes input/authority/acknowledgments and permits at most one
non-ACK asset chunk before pending video. That does not remove current-record
head-of-line blocking or prove sustained 4K60 on a slow link. Kind5 admission
bounds rich records to roughly 64 KiB; the existing generic frame reader can still
allocate its media maximum before kind validation. Original video, helper and
OS decoder allocation bounds therefore continue to apply.

Pure codecs, malformed-input/overflow/window/hash tests, concurrent-direction
race checks and blocked-writer encrypted pipe fixtures are local qualification
only. Rich native publication, installed consumers and actual paired PNG/files
copying remain pending separate acceptance. Earlier text qualification and its
three-second barrier do not establish that rich behavior.

## Linux viewer staging candidate

The Native desktop viewer candidate workflow builds the Linux amd64 viewer on
Ubuntu 22.04 using the pinned standalone Dockpipe CLI, public DisplaySDK,
Qt Multimedia kit, and the package's media input lock. It runs the native X11
fixtures under Xvfb and exports a one-resolver store with its producer receipt
and dependency inventory. The viewer workflow runs independently of VM catalog
qualification. The protected catalog promotion accepts a `viewer_candidate`
from a successful owning dev run with exact artifact, package and manifest pins.
A source-bound distribution review must approve those same bytes. The catalog
request promotes `resolver/dockpipe.desktop.native` only in its `linux-amd64`
policy. The existing native consumer installs the viewer before the staging
Marketplace pointer changes.

## Distribution and license qualification

See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). Native artifacts retain
license texts and a hash inventory. Exact Qt/FFmpeg binary-to-source provenance,
LGPL relinking/source delivery, platform signing/notarization, H264 patent
obligations, full native closure and paired UI/media/input/cleanup acceptance
must be qualified before publication. A locally hash-pinned binary and upstream
header source alone do not close those distribution requirements.

## In-process native worker adapter (review candidate)

Darwin production also builds a Foundation-only Mach-O module implementing the
independent `dockpipe.native-worker-sdk/v1` C ABI. It has no Qt dependency and does
not alter DisplaySDK 1.0. The worker algorithms remain package-owned. A qualified
Dockpipe App loads this module in its own dedicated host-mode process, so native
capture and explicit permission actions use the App's identity. The compatibility
standalone worker remains available; its existing App and runtime manifest are
preserved separately.

The worker SDK header is an explicit, digest-pinned **unpublished review artifact**
in `dependencies/inputs.lock.json`. Supply that artifact in the offline input
folder; sibling source includes are not a supported dependency. This candidate is
not a published SDK or a distribution-qualified App.

The opaque profile passed by the host is strictly one of:

```json
{"schema":"dockpipe.desktop.native.host/v1","operation":"status"}
{"schema":"dockpipe.desktop.native.host/v1","operation":"request-screen"}
{"schema":"dockpipe.desktop.native.host/v1","operation":"request-input"}
{"schema":"dockpipe.desktop.native.host/v1","operation":"worker","channel_helper":"/absolute/owned/helper","manifest":"/absolute/private/module-runtime.json"}
```

The host supplies the frozen CLI and state directory through the ABI, never
through profile overrides. Status does not request permission. Only the explicit
local request operations may call permission APIs, and none starts capture or
opens a remote session. Normal worker startup, remote directives and discovery do
not request permission. The existing owner policy, exact reference, helper hash,
authority leases and cleanup proofs still govern actual work.

The producer signs the module before writing `assets/worker/worker-factory.json`
and `assets/tooling/provider-module-runtime.json`. The provider artifact is the
final signed **module image** hash. At entry the module resolves itself with
`dladdr` and verifies those bytes against the private runtime manifest; it does
not substitute the host App executable. A host may load an admitted byte-for-byte
private image snapshot with the same digest. Helper and runtime paths remain
explicit profile references, independent of the loaded image's directory.

The C entry runs once on the main thread, polls the host's stop callback, and
services the native run loop during bounded shutdown. It installs no global
signal handlers. Pipe writes mask and consume only a newly generated SIGPIPE on
the calling thread, preserving the host's disposition and prior mask. Notification
observers are removed at return. A cancellation result never asserts cleanup:
`cleanup_verified` remains an independent proof bit. Unknown native state remains
retained for process life. There is no unload/dispose ABI; the host must retain the
module and end the dedicated process when cleanup is unknown.

Portable fixtures cover strict profiles and host pipe safety. Native component
fixtures load the module and exercise invalid ABI/profile/thread, pre-cancelled
entry and one-shot admission, without invoking any permission/status/capture/input
API or starting the CLI worker. Those fixtures qualify the component boundary,
not App consent, capture, input, packaging distribution or actual deployment.


A worker with denied initial permissions stays idle with unavailable capabilities;
it does not request consent or exit merely because permission remains denied.
Changes to effective, owner-bounded readiness retire the frozen catalog. An idle
worker first verifies its CLI bridge stopped, then returns cleanly so an external
supervisor may publish a fresh catalog. With an owned session, either permission
gain or loss first requires existing native cleanup and exact original-reference
Forget. Authority is never widened in place, and cleanup uncertainty never becomes
a clean restart. Owner-policy or runtime-manifest changes remain fail-closed errors.


Native stop helpers take shared state by value before creating escaping
Objective-C blocks. Encoder finalization, the stop watchdog and delayed capture
stop completion retain that state after their caller closure returns. The native
ownership fixture exercises the actual private helpers with a blocked synthetic
encoder finalizer and fake capture-stop selector; it invokes no privacy, capture
or input APIs. The original reference-parameter shape is rejected before a freed
state is dereferenced. Deadline, exact Forget and unknown-cleanup rules are unchanged.


Established authenticated helper EOF is a close event. It enters the existing
native stop, signed report, exact original-reference Forget and verified bridge
retirement path before Success. Pre-Ready exit stays a failure. Buffered records
are validated before exit classification: truncated/malformed records and input
outside the frozen mode remain failures; buffered input is never posted after a
known helper exit. No catch-all exception is converted to Success.

Failure-only stderr diagnostics contain fixed stage/reason enums, cleanup proof
bits, and separate known/verified capture, encoder and input-release components.
They include no paths, profiles, session identities, raw exception messages, media
or input. Unexpected exceptions may add only a bounded-message SHA-256 digest.
First failure and unverified component observations remain sticky across cleanup
retries/late callbacks. Backend diagnostics observe existing results; capture,
encoder, input, watchdog and authority behavior are unchanged. This source slice
requires separate native module build/qualification before activation.


Native-to-Core reports are published only when the exact serialized report
changes. Every incoming directive still undergoes the same frozen-reference,
mode, ACK, lease and signal validation, and every applicable native/TLS lease
renewal still runs. Binding, ACK, Running and terminal Closed/Unknown transitions
are preserved. The reliable local pipe and Core's retained exchange report carry
retries; duplicate Running JSON is not a fresh lease acknowledgement. The cache
retains at most one existing-limit metadata line and commits only after a full
write. It never journals raw input or media. This bounds stale IPC reports ahead
of cleanup while preserving exact native/child/Forget/bridge proof requirements.

### Optional asynchronous viewer close

The returned display surface QObject offers `requestClose()`, the read-only
`closeReference` and `closeCompletionBudgetMs` properties (12,000 ms), and the
`closeCompleted(QJsonObject)` signal. A consumer captures the exact prepared
reference and connects before requesting close, disables its input UI, and keeps
the surface and module alive until completion or that bounded deadline. The
package immediately stops input, media delivery and lease renewal. Owned local
processes and the decoder stop concurrently while it observes the authenticated
control connection. No reconnect or new operation is started.

Completion has exactly `{schema: "dockpipe.desktop.close/v1", reference,
outcome: "closed" | "unknown"}`. Only an exact authenticated Core `closed` view
with zero lease and no signals, together with local cleanup, yields `closed`.
Unknown, revocation, transport exit and timeout retain uncertainty. A completed
result can be delivered again for an idempotent subsequent request. An empty
reference before preparation can only yield `unknown`.

After the hook completes or expires, the consumer calls DisplaySDK 1.0
`closeDisplay()` and deletes the surface. That published method remains a
synchronous cancellation/join boundary, with callbacks stopped before return.
Older hosts that do not use the optional hook receive its existing short close
observation; delayed proof stays available through the retained exact reference,
and is never inferred from process exit. The hook does not change the SDK ABI.

The display plugin receives the canonical catalog `packageRoot` through
DisplaySDK 1.0 `initialize`, and resolves its contained
`assets/display/display-factory.json` from that root. The descriptor's helper path
remains relative to the package root. Passing the assets subdirectory in place
of the package root is rejected; no alternate initialization path is inferred.

The viewer uses a widget-scoped blank cursor only over the aspect-fitted live
remote image. The native sender includes its own cursor in captured frames.
Letterboxing, leaving, losing focus and beginning close restore the inherited
local cursor; no application-wide cursor override or pointer movement is used.
First-frame delivery and layout resize re-evaluate the last hover position even
without another mouse event.

### Capture and presentation preferences

Initial display options may include `capture: {quality, frame_rate}` and
`display: {scale, hide_cursor, swap_control_command?}`. Omission preserves high/30, fit/true and no modifier swap.
Capture quality presets are low (1280x720, 1 Mbit/s), balanced (1600x900,
2.5 Mbit/s), high (1920x1080, 4 Mbit/s), and ultra (3840x2160, 16 Mbit/s).
Rates are exactly 10, 15, 24, 30 or 60 fps. The native sender preserves aspect ratio, even dimensions and the
original display geometry. Preferences never widen view or input authority.

The private helper channel v2 requires pinned TLS and the exact-reference HELLO,
then encrypted profile request, echo, confirmation and final acknowledgment.
Both endpoints validate the same exact profile before either one-shot readiness
receipt or native capture. Legacy helpers, missing/extra profile fields, changed
agreements and unsupported values fail explicitly. The agreed profile controls
SCStream dimensions/rate, VT encoder bitrate/rate and encoded-frame duration.
Capture preferences are immutable within a session and require an explicit new
connection to change; no reconnect or fallback is automatic.

The surface QObject exposes `setPresentationOptions(QJsonObject)` and read-only
`presentationOptions`, using the direct exact object `{scale: "fit" | "actual",
hide_cursor: bool, swap_control_command?: bool}`. The optional swap defaults off; Alt and the physical escape chord retain their meaning. Invalid updates leave the property and active session
unchanged; consumers can compare readback. Presentation updates do not change
the capture profile or establish a new connection. Fit preserves aspect ratio.
Actual uses one decoded image pixel per logical viewer pixel, centered when it
fits, with local scrollbars when larger. Scrollbar offsets retain normalized
coordinates over the full image; scrollbar/gutter pixels never become remote
input. Cursor hiding can be disabled and remains scoped to the live image.

The read-only `captureObservation` surface property is empty until authenticated
channel readiness and a decoded frame. While active it contains exactly
`{capture: {quality, frame_rate}, width, height}` using actual decoded QImage
dimensions. Beginning close clears it. These bounded metadata fields permit
verification of the agreed profile and dimension limits without retaining image
content. Frame rate and bitrate are encoder/capture targets; this property does
not claim measured frame throughput or a strict bitrate upper limit.

### Optional 4K, 60 fps and hardware decoding

The `ultra` capture preset caps 3840x2160 at a 16 Mbit/s encoder target. All four
qualities accept 10, 15, 24, 30 or 60 fps; the default remains high/30. Capture keeps
native aspect ratio/even dimensions and never upscales a smaller display. Level
caps are 4.0 for non-ultra through 30 fps, 4.2 for non-ultra 60, 5.1 for ultra through 30,
and 5.2 for ultra 60. Matching endpoints reject unsupported profile values before
readiness; they never silently downgrade. The 2 MiB encoded-record limit remains.

Every raw SPS is checked before decoding against frozen visible/coded geometry,
constrained-baseline syntax, level/DPB/VUI bounds and the envelope. A keyframe
must include checked sequence parameters. CPU max_pixels and software-buffer
callbacks use the frozen profile; hardware format/frame pools have separately
bounded codec padding. Two decoder threads drain a bounded four-picture queue.
When a newer compressed picture is waiting, the decoder can retain its reference
pictures while omitting stale presentation work. The latest presentation output
occupies one slot. An encoded sequence gap discards dependent P-frames, requests a keyframe
and awaits a checked IDR to reset a healthy decoder's reference pictures while
retaining its device and codec; an expensive hardware flush is not required. Actual codec failures still retire the failed decoder before recovery.

Initial local-only `decoder` is software (default), auto or hardware, frozen for
that connection and never sent as remote authority. Auto may fall back once to
software and reports why; forced hardware never silently falls back. CUDA/NVDEC
is tried before VAAPI using existing host drivers; no driver installation occurs.

On Linux/X11 with CUDA NV12 output, CUDA/OpenGL interoperability copies the Y and
UV planes directly into two GPU textures. A shader performs range-aware BT.601
or BT.709 conversion and scaling. The retained AVFrame keeps its CUDA device and
decoder surface alive until the GPU copy completes. The renderer releases its
registrations and device reference on reset. Unsupported interop falls back to
the CPU presentation path while retaining the selected decoder policy.
The optional `gpu_presentation` observation reports the actual renderer, frame
count, device-copy wall time, and asynchronous shader timing samples.
`decoderStatus` is empty before a decoded frame or after close, then reports
`{requested,backend,fallback_reason}`. Actual CUDA/VAAPI means a real hardware
frame was validated for presentation. Fixed optional
`decoderFailure` codes hardware-unavailable/hardware-failed remain readable during
failure/close for the host's localized error; no raw driver logs are forwarded.

`captureObservation` also reports decoded frame count, first/last source PTS and
actual decoder elapsed microseconds. The probe labels a decoded-rate observation
only after at least 3 frames and 2 seconds of both timing spans; otherwise it reports
insufficient measurement/null. Configured 60 fps is not measured 60 fps, and accepting
an ultra profile on a smaller physical display is not actual 4K capture. Bitrates
are encoder targets, not strict traffic upper limits. Hardware transfer/color
conversion and widget painting are not claimed to be zero-copy.

### Explicit virtual display source (source candidate)

Capture preferences may additionally select `display_source: "virtual"`. Omission
preserves the existing-display behavior and its canonical two-field profile.
A present source accepts only `"virtual"`; null, empty, `"existing"`, and unknown
values fail explicitly. The source is frozen with quality and frame rate in the
authenticated channel request, echo, confirmation and final acknowledgment before
native readiness. A peer cannot remove or substitute it during negotiation.
Older strict helpers reject the added field before capture or input readiness.

Virtual display dimensions come only from the selected quality preset: low
1280 by 720, balanced 1600 by 900, high 1920 by 1080, or ultra 3840 by 2160.
No arbitrary dimension, display ID, mirror target or fallback source is accepted
from the caller. Virtual frames must match the selected preset exactly; existing
display capture retains bounded aspect-preserving downscaling. The media envelope
and decoder SPS/geometry/level checks remain unchanged; display-source identity
belongs to the frozen channel profile rather than caller-supplied frame bytes.
Source and fixture preparation alone do not qualify native display creation,
permission onboarding, capture, control or cleanup on a target OS.

The native adapter adds a display to the current logged-in desktop; it does not
create a separate user session or private workspace. The four preset pixel modes
use 60 Hz with hiDPI disabled; capture frame rate remains an independent setting.
Creation uses a guarded private macOS API and is qualified only on the tested OS,
not every OS supported by existing-display capture. It needs no BetterDisplay and
never sets primary-display, mirror, or physical-display modes. WindowServer may
replace its headless fallback; restoring an identical fallback ID is not promised.
Virtual display creation and mode selection belong to one dedicated child of the
same Dockpipe app host. The session worker keeps capture, encoding and input;
the display child receives no capture/input grant or signaling bridge. Native
cleanup verifies capture, encoder and held-input teardown before stopping that
child. It then requires child reaping and independent public display absence.
A child acknowledgment, exit code or released object alone cannot prove cleanup.
Failed or uncertain removal retains unknown ownership. Existing capture and app
permission identity remain unchanged.

The local worker profile may add `virtual_display_host_template: {path, sha256}`.
It pins a private full native-host descriptor for the same module, bundled CLI,
state and app identity. Only its opaque profile is replaced for the display child;
production team restrictions or the explicit preview host hash are preserved.
Missing or invalid template configuration rejects Virtual before display effects;
legacy four-field worker profiles still support Existing. Template preparation
does not change consent or grant desktop access.

The separate lifecycle probe requires an explicit `--native-lifecycle` invocation
and is never part of automatic package tests. Developer-parent child-owner probes
qualify the native ownership path; production worker admission, paired capture,
input and authoritative session closure require separate live checks. This source
candidate requires native ownership checks before installation, followed by paired
checks of the installed worker.

### Host-owned Keep desktop awake preference

The native package offers **Off**, **While connected**, and **While remote
desktop is enabled**. These prevent idle system and display sleep using two
owned public IOKit assertions. They do not unlock the Mac, bypass its lock screen,
change remote grants, simulate activity, or override explicit sleep. Off is the
missing-file default. This is a saved setting on the sender, separate from the
viewer's capture, quality and connection preferences.

The package owns the private `desktop-power.json` file in the host-supplied state
directory, with exactly `{schema: "dockpipe.desktop.power/v1", mode:
"off" | "connected" | "enabled"}`. Writes replace a bounded owner-only file
atomically. Core's `desktop.json` remains unchanged. Invalid settings release
owned assertions and fail explicitly; saving a setting does not itself prove an
assertion is active.

The same signed Dockpipe App exposes package-owned native host operations through
its existing `--native-worker-host --request <descriptor>` entrypoint. The opaque
profile schema remains `dockpipe.desktop.native.host/v1`:

- `power-settings` accepts only schema and operation. It shows a native three-choice
  Save/Cancel dialog. It never requests permissions or opens a remote session.
- `configure-power` additionally accepts one strict `mode`, for operator-controlled
  provisioning. It reports the saved mode, not a fabricated running assertion.
- `power-agent` accepts schema, operation, `channel_helper`, `manifest`,
  `desktop_provider` and `desktop_service_label`. It is a persistent companion,
  paired with the desktop LaunchAgent by setup/teardown. Its own loaded image and
  helper match its manifest; the desktop owner independently matches the exact
  supplied provider tuple. They may be different qualified module revisions.

The companion owns assertions only for Enabled, while the independent desktop
owner permits viewing and the selected desktop LaunchAgent remains registered.
It watches saved policy every 250 ms and checks service registration through a
bounded read-only `launchctl print` at most once every two seconds. Off releases
promptly; owner changes, malformed settings, cancellation and failures release
and report their outcome. The companion has a separate private state lock and
never opens a control/media bridge or renews a session lease. It completes the
same background accessory AppKit lifecycle as the worker.

The session worker owns assertions only for Connected after capture is running.
It holds them through held-input, capture and child cleanup, then releases them
before a successful local cleanup or Core closed report. Release uncertainty
keeps cleanup unknown. Off owns neither pair; Enabled belongs solely to the
companion, avoiding duplicate ownership by the session worker.

Older workers cannot implement Connected. An upgraded worker publishes a private
`desktop-power-capabilities.json` record whose provider must exactly match the
current desktop owner. Until then, the native dialog disables Connected with an
explanation, and noninteractive Connected configuration fails. Enabled and Off
can be provisioned with the companion beside an older worker. Setup must start
and stop the companion with the desktop service, and independently verify actual
assertion state; a viewer-local selection or saved-mode receipt is insufficient.
No separate consent application or generic Core/SDK API is introduced.

If WindowServer initially selects another mode, creation may select the exact
requested 1:1 pixel/logical mode at 60 Hz on that newly owned virtual display.
The retained object, fresh display ID, vendor/product/serial and preserved physical
topology must all match before this single public mode-selection attempt. Already
correct modes need no setter. Cancellation or expiry blocks a pending selection;
a failed or partial setter never grants readiness or triggers a retry. Exact
current-mode readback and verified display removal remain required. This changes
no primary-display, mirror, origin or physical-display mode settings.


## Separate Flatpak viewer candidate

The private [KDE 6.10 producer](tooling/README-flatpak.md) builds a separate
Linux amd64 viewer against the matching Flatpak compiler, DisplaySDK and media
runtime. Native source manifests remain native. Only a generated resolver that
passes both SDK and Platform fixtures declares `platforms: [flatpak]`.
No Flatpak provider archive or matching DisplaySDK has been published, and no
installed Dockpipe GUI or paired end-to-end session is qualified by these tests.
