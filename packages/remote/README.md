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

## Explicit standalone cloudflared update and recovery

The resolver now implements `dockpipe.dependency.change/v1` through its packaged
`dependency-change` workflow. The initial apply method is **Linux private
standalone binaries only**. The operator attests that the target is independently
owned; it must be named `cloudflared`, singly linked, owned by the current user,
mode 0500 or 0700, and inside a private directory. The executable mode is preserved through update and recovery. System installations,
package-manager files, services/containers/bundled distributions and non-Linux
replacement are not qualified. Use the original installation owner for those
methods. No automatic elevation, download, package-manager call or restart occurs.

Prepare an exact request after obtaining the correct native standalone release
from [Cloudflare's downloads](https://developers.cloudflare.com/tunnel/downloads/)
and verifying its checksum through trusted release evidence. A supplied checksum
proves byte integrity, not publisher authenticity. This manual artifact lane does
not replace signature or platform trust verification and must not be used for
an artifact of unknown origin. Manager installations must use the same manager,
as required by [Cloudflare's update guide](https://developers.cloudflare.com/tunnel/guides/update-cloudflared/).

```json
{
  "schema": "dockpipe.dependency.change/v1",
  "resolver": "dockpipe.cloudflare.remote-edge",
  "provider_version": "0.3.1",
  "machine": "local",
  "parameters": {
    "owner": "private-standalone",
    "target": "/private/tools/cloudflared",
    "artifact": "/private/downloads/cloudflared",
    "expected_sha256": "<installed binary SHA-256>",
    "sha256": "<verified artifact SHA-256>",
    "expected_version": "2026.10.2",
    "version": "2026.10.4",
    "pin": "2026.10.4"
  }
}
```

The paths are placeholders, not default installation locations. Both input files
must be private regular files in private directories. No package/host lock is
inferred from a producer lockfile. This lane cannot adopt an ecosystem-managed
installation or override its locks. The optional exact `pin` must equal the
candidate version. Stable versions only; the candidate must be newer and meet
the advisory floor. Correct architecture is checked by executing the verified
staged candidate's bounded version probe before mutation.

```sh
dockpipe dependency update --request change.json --dry-run
dockpipe dependency update --request change.json --approve APPLY_ID
dockpipe dependency recover --request change.json --dry-run
dockpipe dependency recover --request change.json --approve RECOVERY_ID
```

The returned approvals bind every input and the operation; apply approval cannot
approve recovery. The provider locks the target's private update state, copies and
hashes the candidate, preserves and synchronizes the previous binary, journals
before replacement, rechecks expected installed bytes, atomically renames and
synchronizes the directory, and verifies the installed version. Interrupted
replacement is reconciled by comparing installed bytes to both approved digests.
Unknown bytes or corrupt backups are preserved for manual investigation. A failed
postcondition attempts bounded safe recovery and returns a failure even if the
old binary was restored. Recovered transactions cannot silently apply again.

Journals and backups remain beside the target under
`.dockpipe-cloudflared-update/`, with separate transaction identities and a shared
lock. Retain this private evidence until activation/recovery is resolved. A later
transaction can proceed only after the active transaction reaches a reconciled
terminal state. Repeating an installed transaction checks its actual current
version instead of replacing again. A pending/ambiguous transaction blocks new
updates; no blind replay or background recovery agent is introduced.

Rollback to a version below the advisory floor is blocked. For a deliberately
approved emergency exception, add
`"emergency_recovery": "allow-vulnerable-rollback"`, review recovery again, and use
the **new recovery approval**. This policy is separately approval-bound while
preserving the original installation transaction identity, and its use is recorded
in the recovery journal. This restores vulnerable bytes and does not remediate
exposure; prefer a verified forward fix via the installation owner.

Success is `installed-activation-pending` or `recovered-activation-pending`.
Existing connector processes retain their current executable images. Tunnel IDs,
credentials, config and connections are untouched; no running-version, connector
readiness, broker reachability, application TLS or end-to-end health claim is
made. Activation and live recovery need the separately approved broker/service
maintenance and redundant/out-of-band management work in TASK-050/052. In
particular, restarting the connector carrying a remote update is not supported.
