# Immutable native VM catalog candidate

This package-owned producer owns an exact eight-entry catalog overlay: five VM
family installables and three source compatibility workflows. It never exports
runtime core or infers whole-catalog ownership. Input admission is separate from
native producer qualification and immutable publication.

The native GitHub prerelease is
`Dockpipe-Industries/dockpipe-packages/catalog-native-candidate-<full source commit>`.
Each actually built platform supplies `dockpipe-catalog-<platform>.tar.gz`.
The archive is flat: `packages-store-manifest.json` plus exactly referenced
versioned `dockpipe-*.tar.gz` files. Manifest schema/platform/positive sizes and
hashes match the core ingestion contract. The generated publication plan includes
the exact `catalog:{source_commit,owned,artifacts}` core lock overlay.

## Locked inputs and execution

`release/native-store.lock.json` is the admitted source template used by CI.
Its public CLI, four-platform Display SDK verification, Guest Tools release,
baseline/source compatibility identities and official acquisition recipes are
independently pinned. `release/native-store.lock.example.json` remains an
intentional incomplete example and is not a runtime or CI input. Every
profile requires source, core CLI, a released compiled runtime-core archive, DisplaySDK, Qt Widgets, GIO, native toolchain,
private broker/provenance, the public Go SDK pair, and qualified Windows Guest
Tools inputs. Direct dependencies bind an exact public GitHub release URL,
SHA256, size, archive type and root. An optional `native_inputs` profile derives
Qt, GIO and the private broker/provenance from the exact official multiarchive
recipes in `release/native-input-recipes.json`; it replaces those four direct
roles rather than accepting two competing sources. The tracked template keeps only its own `source_commit`
and source-asset fields null: embedding its current commit/hash in that same
commit would be self-referential. `prepare_ci.py` validates every dependency pin
first, fetches the accepted full-commit public source archive once, computes its
actual transport identity and shares one effective lock/archive across all jobs. Network acquisition is
explicit; existing content-addressed cache files are rehashed.

The Guest Tools component has its own explicit source commit, which may differ
from catalog source on a later reviewed reuse. A separately pinned public
`guest-tools-candidate-manifest.json` must bind its committed Windows source,
native build/CTest/installer checks, toolchain lock, ZIP and unchanged delivery
descriptor. Local development paths or receipts cannot satisfy it.
The producer reuses the owning `build_windows.py` and materializer contracts.

`bootstrap.py` acquires the locked CLI and enters `config.yml` through Dockpipe.
The workflow uses native `run:.py`; its CLI therefore must include that public
primitive. `tooling/bootstrap_runtime_core.py --output <fresh task-global-root>` must first seed its verified compiled
archive into task-private `DOCKPIPE_GLOBAL_ROOT/packages/core`. This producer
requires that explicit output and independently checks its locked filename/hash/size
and that no competing core archive is present. It does not duplicate the shared
bootstrap acquisition/installation implementation. A CLI-only bootstrap cannot
satisfy this dependency; no core source copy or sibling fallback is permitted.
The isolated source archive must declare
`build.source.script: assets/scripts/build-source.py` before this lane accepts it.
Before source compilation, `tooling/catalog_go_dependencies.py` authenticates the
pinned public Go SDK, checks the effective Go target, and acquires the build/test
module graph into the Go cache. It uses a temporary modfile and only checksums
already present in catalog source or the verified SDK; source mod/sum files stay
unchanged. CodeQL uses the same acquisition helper. The later
`tooling/catalog_go.py` build independently validates the SDK and keeps
`GOPROXY=off` and `GOSUMDB=off`.

The producer invokes public `package build source --only vm`, compiles VM
workflows, exports workflow and resolver stores separately, then combines only
`owned` entries into the deterministic flat archive. The existing VM source hook
runs the native DisplaySDK/Qt build and closure checks. No sibling core or
PipeLang source fallback exists.

`previous_artifacts` is the reviewed filename→SHA256 map from retained baseline
store metadata. Any changed bytes under a previous name fail; package versions
must be bumped in approved source. An outer candidate tag is not a version bump.
Core ingestion independently enforces that rule too.

## Admitted platforms and native gates

The source template requires Linux amd64, Windows amd64 and Darwin arm64.
Each target must pass its real native preparation, build, tests and immutable
publisher checks. The Mac release baseline is macOS 15/Xcode 16.4/SDK 15.5;
prior paired-host macOS 27 evidence does not qualify that lane. Linux arm64 and
Darwin Intel have no admitted catalog profiles. No cross-build or other host's
store silently fills a missing target.

The native recipes use Qt 6.10.3 and exact upstream payload hashes, sizes,
package identities and license inventories. The Linux profile requires Ubuntu
22.04 and GNU 11.4.0; it does not silently select the legacy Ubuntu 24.04 lane.
The Windows profile requires MSVC 19.51.36260.0 and Windows SDK 10.0.26100.0.
Every run rehashes its declared archive bytes before bounded extraction.
Acquisition alone does not establish native compiler or producer qualification.

The extractors admit bounded regular data from 7z, Debian ar/data.tar, MSYS2
pkg.tar.zst and source tar.xz archives. No package installation scripts run.
Contained aliases are graph-validated then flattened to regular bytes inside a
fresh prefix; escaping, missing and cyclic aliases fail. Windows paths remain
case-insensitive; Linux preserves legitimate case-distinct documentation names
and applies the Ubuntu merged-/usr layout only to Debian members. Entry count,
metadata, individual files and total expanded bytes are bounded. Qt redirects
are restricted to exact reviewed hosts from official Metalink metadata. The
hash-locked per-platform Python extractor wheel sets are installed with
`--require-hashes --only-binary=:all:`.

Linux recipe builds explicitly select `DOCKPIPE_VM_LINUX_DEPENDENCY_MODE=prefix`
and an exact package/license/library receipt. The display producer checks every
admitted byte and resolves non-system GIO dependencies only from the declared
prefix. Missing or invalid prefix metadata never falls back to host dpkg.
Existing `host-dpkg` behavior remains a separate explicit profile. Every declared
package copyright record must be present in the exact prefix receipt before
linking; an extracted dependency is not represented as rebuilt source.

`release/native-compiler/windows-hosted-19.51.json` declares Windows compiler
acquisition as `hosted-exact`. The supported public `windows-2025-vs2026` runner
label supplies the preinstalled VS 2026 Enterprise toolset. This is a rolling
host image, not an immutable or hermetic compiler bundle. Actual image details
are recorded as observations. No installer, channel download, system install,
repair or compiler fallback runs in this profile.

The source-bound descriptor records the public SDK 79bb110 metadata identity,
compiler VSIX and desktop CRT VSIX URLs/hashes/sizes, and observed x64 PE file
identities. Those payloads establish exact compiler and runtime byte references,
not installer provenance. Only admitted preinstalled hosted files participate in
this profile; channel downloads and installer execution are outside its contract.

`prepare_msvc.ps1` requires the owning GitHub-hosted Windows job and selects
`Program Files/Microsoft Visual Studio/18/Enterprise/VC/Tools/MSVC/14.51.36231`.
It verifies the exact cl.exe hash/version, all ten desktop CRT hashes/sizes/
versions, x64 PE headers and valid Microsoft Authenticode signatures. It rejects
reparse points and uncontained paths. Explicit `vcvars64.bat` arguments select
that toolset and SDK 10.0.26100.0. Previous compiler include/library and vcvars
sentinel variables are cleared before initialization. Post-initialization checks
admit only the selected compiler/link/lib/dumpbin and SDK rc/mt command paths.
The receipt records their actual hashes and image observations, with
`acquisition_mode: hosted-exact`; it never claims signed-installer execution.
Native preparation rejects missing or changed compiler/runtime/SDK files. The
recipe explicitly propagates `CMAKE_GENERATOR=Ninja` to the fresh owning source
hook so CMake uses admitted `CC`/`CXX` rather than a default Visual Studio toolset.

The direct immutable `inputs.toolchain` archive remains supported. A profile may
instead declare `toolchain_recipe: {path, sha256}` referencing one contained JSON
file in the exact acquired catalog source archive. The modes are mutually
exclusive. The three source profiles in `release/native-toolchain/` pin actual
upstream Go 1.26.10, CMake 3.31.6, Ninja 1.12.1 and pkgconf 2.3.0 inputs; Linux
also pins the eighteen official GNU 11.4/Ubuntu 22.04 compiler/runtime packages.
This creates a private ephemeral build prefix rather than inventing a public
compiler bundle URL. Compiler binaries are not included in catalog artifacts.

The Linux dispatcher retains the separately reviewed recipe. The Windows
dispatcher first requires the hosted-exact Microsoft admission receipt and exact SDK 10.0.26100.0 resource/manifest tools. It extracts only
bounded regular tool files, installs the hash-pinned Meson 1.7.2 wheel offline in
a disposable venv, and builds pkgconf with that MSVC compiler and Ninja. It never
installs a compiler or chooses a host pkg-config fallback. The native probe checks
tool versions, a C++17 shared-library/string consumer and isolated .pc metadata.
GNU requires glibc 2.35 and a dynamic libstdc++ dependency; MSVC requires its
matching dynamic CRT. Prefix inventory and license hashes, acquisition identities,
and Microsoft compiler/SDK receipt hashes are bound into candidate provenance.
The publisher validates and publishes each platform's toolchain receipt as an
additional immutable asset. Every native run must produce a successful ABI
probe receipt before package artifacts may enter publication.

The Darwin arm64 dispatcher pins official Go/CMake/Ninja bytes and builds the same
pkgconf source with the explicitly selected Xcode 16.4 compiler and SDK 15.5.
It requires the macos-15 arm64 runner, AppleClang 17.0.0.17000013 and deployment
floor 15.0. Job-local `DEVELOPER_DIR` selects Xcode without changing machine
selection. Exact compiler paths/hashes, SDK identity, dynamic libc++ admission
and the native shared-library consumer are recorded. The CMake application is
extracted with bounded regular files and graph-validated contained framework
aliases; no ambient Homebrew compiler/tools or source siblings substitute.
Each run must produce native macOS 15 recipe/ABI and installed-product receipts.
Evidence from another macOS baseline does not satisfy those gates.

One final overlay owns eight entries: the three qualified source compatibility
workflows plus qemu, linux-vm, windows-vm, vm-manage and vm-resolution. The source
repairs come from the exact published `30859228a77ba20a23a4b69ab81aae133d6ccfe9`
overlay, with its independent source/hash/size pin. Their inner archive bytes are
preserved verbatim; only the outer consuming store target changes. The single VM
source hook and workflow-family compile produce the five new VM installables.
All eight identities must appear in the final union, so promoting native VM
packages cannot remove the earlier dependency repairs. Existing core lock wire
and ownership bounds are unchanged.

Each run admits the exact public CLI/DisplaySDK/Guest Tools pins and source hook,
then enforces native producer and public readback gates. The separate incomplete
example deliberately has unresolved dependency slots and cannot run this lane.

`publication_plan.py` rechecks every emitted archive, manifest, inner artifact,
platform and ownership set. `publish_candidate.py` implements the actual GitHub
candidate boundary. It accepts only an exact owning-repository `dev` push and
explicit `accepted_platforms`; dispatch is build-only. It binds every native
candidate to the same committed source and effective lock, checks the current
`dev` head and tag, inventories a release by numeric ID, creates a draft
prerelease or resumes its verified subset, and uploads only missing assets.
Changed or extra remote bytes fail. A published incomplete release cannot be
repaired or extended in place. There is no clobber, delete, force or latest
promotion operation. An ambiguous network result stops; a later explicit run
must re-inventory server state before resuming.

Publication uploads the exact platform archives, declared toolchain receipts and `native-store-release.json`,
then verifies tag/source, exact asset inventory, unauthenticated public hashes
and sizes, and that GitHub's latest release remains unchanged. Only then is
`publication-receipt.json` written. Public reads use bounded HTTPS curl with
configuration and GitHub token environment removed. A failed readback does not
attempt deletion or rollback. Existing generic R2 upload scripts are not used.

The CI `inputs` job validates and exports the single effective source descriptor;
native jobs use only its explicit platform matrix and the shared compiled-core
bootstrap. The final job has the sole `contents: write` permission and requires
all accepted native builds. It publishes only on `push` to `dev` in the owning
repository; manual dispatch cannot publish. A successful input or build-only
run is not a publication receipt.

Focused contract tests (the 7z fixture requires the locked extractor environment):

```sh
python3 -B -m unittest discover -s workflows/package/package-native-store-candidate/tests -v
```

They prove input/packaging contracts with synthetic archives, not Qt rendering,
full installed launcher acceptance or native store distribution. Fresh hosted
native toolchain/ABI execution, package builds and installed host/provider
acceptance remain required before publication. Dependency lock admission does
not establish those outcomes.

The explicit-prefix package boundary has its own focused suite:

```sh
python3 -B -m unittest discover -s packages/vm/display/tests -p test_linux_prefix.py -v
```

## Backend and guest acceptance gates

This recipe prepares the display module, private broker and build toolchain; it
does not build or qualify a QEMU backend. Installed provider/module receipts must
bind the actual backend and process identity used by the run. Fresh sessions use
the owner-private schema 2 absolute-input channel described in
[the display contract](../../../packages/vm/display/README.md#absolute-pointer-endpoints).
Existing schema 1 sessions retain their documented input behavior rather than
silently borrowing another monitor. Protocol tests do not replace actual guest
corner acceptance through the installed widget on the selected native platform.
Alternate-resolution acceptance likewise requires configured and guest-observed
geometry, not only a parsed workflow field or successful request acknowledgement.

## Disjoint immutable publication modes

Native candidates use `catalog-native-candidate-<40-character source commit>`.
Source-only compatibility candidates retain `catalog-candidate-<40-character source commit>`,
including all existing public tags and bytes. Each release keeps its existing asset filenames.
The same source commit can therefore qualify both modes without sharing a release or asset URL.

Both producer descriptors and release receipts declare `publication_mode` (`native` or
`source-only`). Both callers explicitly admit that mode to the shared planner and immutable
transaction. Mode, exact tag, native-build flag and receipt class must agree before any
remote read or mutation. Native source/toolchain/build checks and source-only consumer
proof/attestation checks remain in their owning callers. No mode may extend the other
mode's release or replace existing bytes; disagreement fails closed. The standalone
publication planner requires `--qualification native|source-only`.

The example's `accepted_platforms` is the intended required build/publication
set and exactly equals its three profile keys: Linux amd64, Windows amd64 and
Darwin arm64. This field is not evidence of successful qualification. Selecting
Darwin requires its real Mac15/Xcode16.4/SDK15.5 native job and publisher result;
missing, failed or partial platform output cannot publish that candidate. The
example still contains unresolved dependency slots and is not an admitted lock.
Choose a smaller candidate only by explicitly removing both the profile and its
platform entry, preserving exact ownership and all checks for every selected host.


The Windows profile matches the public Display SDK's exact compiler
MSVC 19.51.36260.0 and pins desktop CRT 14.51.36247.0. Qt 6.10.3, GIO 2.90.1
and Windows SDK 10.0.26100.0 remain unchanged. Only the admitted ten-file desktop
CRT is copied beside the disposable C++ shared-library ABI probe. The probe
checks actual loaded CRT module paths and rejects System32 runtime fallback.
Final VM display assets delegate exactly those ten known CRT import names to
the matching desktop host; future CRT imports fail closed. No System32 DLLs are
harvested or copied into package assets.

Portable source/failure tests do not qualify this hosted profile. Clean native
Windows admission, shared-library ABI execution, full package compilation/tests
and matching desktop integration must still pass before immutable publication.
Missing exact image tools fail rather than installing or choosing latest tools.
