# Windows guest services

`windows/` owns the Windows guest-services agent and Guest Tools bootstrap installer.
The QEMU source-build hook builds this standard-library-only module for Windows and
places `dockpipe-guest-agent.exe` beside the resolver's provisioning scripts. Authored
source is never changed or removed by the build.

This agent is separate from `tools/cmd/dockpipe-guest-agent`, which owns the signed
Linux qualification protocol. Neither implementation replaces the other.

`backend-origin.json` records the source paths and SHA-256 values captured during
migration from Dockpipe. The runner's migration replaced adjacent SDK lookup with
the explicitly selected generic SDK or the pinned CLI's `sdk` command. Its package-owned implementation
has since gained installer validation, consistent disk formats and explicit guest
preparation behavior. The source hashes retain the migration baseline; destination
hashes describe the current package postimages.
