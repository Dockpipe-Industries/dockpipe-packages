# Private Flatpak viewer producer

`remote-desktop.build-flatpak` is a separate two-stage development producer for
Linux amd64, KDE SDK/Platform 6.10. `dependencies/flatpak.lock.json` pins the exact
runtime commits, Qt 6.10.3, GNU 15.2.0 and public DisplaySDK header identities.
It does not change the native compiler, dependency lock or source platform list.
The package consumes Platform FFmpeg and Qt; neither runtime is copied into the
candidate. The existing viewer requires X11 physical keyboard mapping for control.
Wayland control and incoming capture/control hosting are not supplied by this
viewer profile.

First prepare the ordinary locked dependency inputs and explicit Go 1.26.10.
The native published DisplaySDK requires GNU 11.4.0 and cannot be substituted.
Produce a separate SDK using Dockpipe's owning `build-display-sdk` workflow inside
the pinned KDE SDK. Its relocated external consumer must pass, its headers must
match this profile, and its release/file manifests and archive must stay together.
Pass the exact SHA-256 of that selected `sdk-release.json`; a development artifact
additionally requires `DOCKPIPE_DESKTOP_ALLOW_DEVELOPMENT_SDK=1`. This explicit
selection is private qualification, not a fabricated published SDK pin.

Run the package workflow from an isolated owning repository snapshot. Build in
the SDK and qualify in the smaller Platform. Supply only the selected source,
locked dependency inputs, Go toolchain and task output as filesystem grants.
Disable network and host execution. Both stages require a task-owned authenticated
Xvfb display: `QT_QPA_PLATFORM=xcb`, `DOCKPIPE_CLIPBOARD_OWNED_XVFB=1`, and that
display's `DISPLAY` and `XAUTHORITY`. Grant its X11 socket and authority file;
never attach these clipboard fixtures to the workstation display. Stop and reap
that owned server after both stages, including on failure.

Set these explicit workflow environment inputs:

| Variable | Meaning |
| --- | --- |
| `DOCKPIPE_DESKTOP_FLATPAK_STAGE` | `build` in the SDK, then `qualify` in the Platform |
| `DOCKPIPE_DESKTOP_OUTPUT` | Fresh absolute task directory for build; same directory for qualify |
| `DOCKPIPE_DESKTOP_DEPENDENCY_INPUTS` | Absolute ordinary locked dependency input directory |
| `DOCKPIPE_DESKTOP_GO` | Absolute Go 1.26.10 executable, with its complete toolchain visible |
| `DOCKPIPE_DESKTOP_FLATPAK_DISPLAY_SDK` | Separate matching SDK archive/release/file-manifest directory |
| `DOCKPIPE_DESKTOP_FLATPAK_DISPLAY_SDK_SHA256` | SHA-256 of its exact release JSON |
| `DOCKPIPE_DESKTOP_ALLOW_DEVELOPMENT_SDK` | `1` only for an explicitly selected private development SDK |

Invoke the pinned absolute Dockpipe CLI in each runtime with the owning workflow:

```bash
"$DOCKPIPE_BIN" --workdir "$PACKAGE_REPOSITORY" \
  --workflow-file "$PACKAGE_SOURCE/workflows/build-flatpak/config.yml"
```

The SDK stage builds and runs the native fixture suite and Go channel tests,
installs the viewer into `OUTPUT/resolver`, and writes a test plan plus provenance.
The Platform stage independently executes that suite without SDK libraries,
loads the installed module with `RTLD_NOW|RTLD_LOCAL`, and verifies the actual
mapped Qt/media libraries originate in the Platform. The plugin fixture checks
the exact Qt/build/compiler admission contract and creates an inactive surface;
it starts no remote connection. Only after all checks pass does the generated
resolver declare `platforms: [flatpak]`. Its checksum inventory, mapped-library
receipt, source inventory, private SDK identity and immutable runtime profile
remain alongside the candidate. Partial/failed output is not an admitted artifact.

The output remains `distribution_qualified: false`. Full installed GUI loading,
actual paired remote view/control, hardware decoder/GPU behavior, signed release
provenance, the published Flatpak SDK, catalog/store adaptation, and publication
remain separate work. An SDK build or offscreen/isolated X11 fixture is not live
platform parity. The native producer and immutable native SDK pins stay intact.
