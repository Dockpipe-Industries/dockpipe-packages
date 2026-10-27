# Remote connectivity resolvers

Provider adapters for `dockpipe remote`. Core owns delivery, pairing, execution, and results.
Resolvers own provider login, endpoint setup, and tunnel configuration. DorkPipe retains scheduling.

`dockpipe.cloudflare.remote-edge` is the Cloudflare adapter (version 0.3.1). Browser login, named tunnel, and DNS setup are implemented;
setup prints progress and pairing guidance and preserves the desktop browser session. Core can
deliver selected workflows, assets and explicit package dependencies to workers that opt in with
`--allow-delivery`. Local loopback delivery is tested; live account and macOS service qualification
remain pending. Other providers can implement the same
contract without changing the broker protocol.

If `cloudflared` is missing, native setup offers the package-declared installer: Homebrew on macOS,
or Cloudflare's signed APT repository on Debian/Ubuntu/Pop!_OS. Installation requires approval and
APT uses sudo. The Flatpak Marketplace artifact includes `cloudflared`; it does not run host APT.

Cloudflare-owned credentials may be owner-readable (`0400`) or owner-readable/writable (`0600`).
Setup preserves those permissions, rejects shared or linked credentials, and reuses an existing
tunnel credential when resuming after a setup failure.

See [Remote nodes](https://github.com/Dockpipe-Industries/dockpipe/blob/master/docs/runtime/remote-nodes.md) for setup, pairing, limits, recovery,
credential handling, build/distribution, and verification.

## Read-only dependency inspection

The resolver profile exposes the optional `dockpipe.dependency.inventory/v1` hook at
`assets/workflows/dependency-inspect/config.yml`. Core supplies the private, new
absolute output file through `DOCKPIPE_DEPENDENCY_OUTPUT` and the bounded machine
identity through `DOCKPIPE_DEPENDENCY_MACHINE`. The hook uses the packaged helper:

```text
cloudflare-edge lifecycle-inspect --output /absolute/new-report.json --machine machine-id
```

This mode never logs in, installs, updates, starts, stops or restarts cloudflared.
It discovers executable candidates from at most 64 absolute PATH entries, known
installation locations and the packaged helper's directory. Symlinks and hard
links are deduplicated, with at most 32 installed executables. Each version probe
runs only `--version`, with a two-second timeout, a 4 KiB output limit and a minimal
environment that excludes credentials and dynamic-loader overrides. The entire
scan has a 30-second budget; partial coverage is reported as a gap.

On Linux, up to 4,096 process-directory entries and 64 readable cloudflared image
handles are inspected through `/proc/<pid>/exe`. No process command line,
environment, credentials or tunnel configuration is read. A fixed PATH binary is
not evidence that a running process uses it. Active version proof uses the process
image handle and checks image identity before and after the version probe. Installed
and running images are reported separately, including replaced or deleted images.
The report is limited to 1 MiB and is created exclusively with owner-only mode.

The package owns this advisory assessment:
[Cloudflare's eMudhra advisory](https://blog.cloudflare.com/emudhra-certificate-compromise/)
names cloudflared `2026.10.2` as fixed. Older stable calendar releases are affected.
The macOS provider floor is conservatively `2026.10.3`; macOS `2026.10.2` remains
unknown because its distribution channel is unconfirmed. Prereleases, vendor
suffixes, malformed versions and unproven active images remain unknown. These
assessments cover this advisory only.

Process namespaces, hidden users, container images, package-manager inventories,
service definitions and undiscovered locations remain explicit coverage gaps.
Renamed executables are not identified as cloudflared process images. macOS and
Windows executable discovery is portable source support only; active process
inspection and native qualification are pending. No missing installation or fixed
installed candidate clears those gaps. This hook provides inventory only; update
planning and execution require a separate implementation and authorization.
