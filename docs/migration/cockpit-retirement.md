# Cockpit source ownership

Cockpit is maintained in the independent
[SpatialPipe repository](https://github.com/Dockpipe-Industries/SpatialPipe/tree/dev/packages/cockpit).
Dockpipe consumes its exact pinned published runtime through the core release
lock and installed-package compatibility tests. This catalog does not publish a
second resolver/cockpit implementation.

The copied 0.1.1 source is retired after its exact 359-file inventory and original
source commit are preserved and verified. Git history retains those original
bytes. Existing release assets are retained unchanged. The current core dev lock
selects the independently qualified 0.1.3 runtime. Historical versions must not
be rebuilt into competing package bytes.

Use the owning product repository for feature, build and test changes. The
complete LauncherSDK and product runtime dependencies remain separate from the
minimal VM DisplaySDK. Core consumes installed artifacts through declared
contracts.

The files under `docs/migration/dockpipe-release` preserve source migration
history. Use the current owning CI and release entrypoints for publication. The
old Cockpit build commands must not publish another package. The native catalog
candidate owns only its declared VM identities. Cockpit compatibility remains
with core's pinned installed-package path.

Source retirement preserves user settings, saved scene data, installed packages,
running processes and the clipboard. Product migration and release publication
use their separately qualified artifacts and authorized runtime operations.
