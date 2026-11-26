# Native desktop third-party notices

The package-authored source is Apache-2.0; the repository LICENSE applies.
The CUDA driver declarations in `source/cuda/dynlink_cuda.h` are retained from
FFmpeg nv-codec-headers n12.1.14.0 under the header's MIT license. The original
notice remains in that file. Its SHA-256 is
`2c74cf9b0ace0503ca18d2c43645d6cb1ec0dd7ea7163f7880bba5141586f275`.
Primary source is
https://raw.githubusercontent.com/FFmpeg/nv-codec-headers/n12.1.14.0/include/ffnvcodec/dynlink_cuda.h.
The viewer loads the existing host CUDA driver for graphics interoperability.
Other third-party source remains an explicit external input. `dependencies/inputs.lock.json`
pins artifact digests, versions and primary source locations. Output inventories
identify exactly which files are distributed.

| Component | Pin / primary source | License / boundary |
| --- | --- | --- |
| Dockpipe DisplaySDK | 1.0.0, commit `d0768d69dc28fba250b3cb19b173457ae3fd3851`; [immutable Linux release](https://github.com/Dockpipe-Industries/dockpipe/releases/tag/display-sdk-1.0.0-linux-amd64-staging.d0768d69dc28fba250b3cb19b173457ae3fd3851) | Apache-2.0; retain the SDK LICENSE. Header-only DisplayPackage/1.0 contract. |
| Qt | 6.10.3 host kit and [Qt Multimedia source archive](https://download.qt.io/official_releases/qt/6.10/6.10.3/submodules/) | The host supplies Qt. Bundled Qt Multimedia dependency stubs carry their corresponding source in `assets/media-sources`, including the license texts. |
| FFmpeg | 7.1.3 runtime from the [Qt 6.10.3 build](https://doc.qt.io/qt-6.10/qtmultimedia-attribution-ffmpeg.html). The build separately uses pinned 7.1.1 public headers. | The Linux package carries the exact FFmpeg source archive selected by Qt, Qt's build and linkage recipes, and FFmpeg notices. The runtime receipt records the loaded library's version and configuration. |
| JSON for Modern C++ | 3.12.0, commit `55f93686c01528224f448c19128836e7df245f72`; [upstream source](https://github.com/nlohmann/json/tree/55f93686c01528224f448c19128836e7df245f72) | MIT; retain LICENSE.MIT for the compiled header dependency. No dependency on libdatachannel is implied. |
| Go standard library/runtime | Go 1.26.10; explicit installed toolchain | BSD-3-Clause and the toolchain's third-party notices. Retain the selected toolchain LICENSE and PATENTS with the helper artifact. No external Go modules. |
| Linux host Crypto | Ubuntu `libssl3:amd64` 3.0.2-0ubuntu1.30, exact binary hash in the input lock | Host-provided OpenSSL 3, Apache-2.0; not bundled and not used by the Go TLS helper. |
| Linux host VAAPI | Ubuntu `libva2`, `libva-drm2`, `libva-x11-2` amd64 2.14.0-1, exact binary hashes in the input lock | Host-provided MIT-licensed runtime libraries, not bundled. This fixture opens no VA display/device. |
| Apple frameworks | Host macOS SDK; Foundation, ScreenCaptureKit, VideoToolbox, CoreMedia, CoreVideo, CoreGraphics, ApplicationServices | Supplied by macOS, not redistributed. Apple SDK and platform terms apply. |

Runtime/system dependencies such as glibc, libstdc++, zlib, GL, PulseAudio and
xkbcommon remain host-provided; this is not a universal Linux ABI promise.
The selected Qt FFmpeg stubs additionally require real host OpenSSL3 Crypto,
VAAPI2/DRM/X11 symbol providers and matching host Qt Multimedia,
Network, DBus and Concurrent modules. Portable source tests do not qualify that
native runtime closure.

The Linux viewer uses shared libraries under `assets/display/lib`. The source
archives, Qt build recipes and their SHA-256 digests are in
`assets/media-sources`. The candidate remains subject to distribution review for
the supported host runtime, applicable LGPL relinking/source delivery and H.264
obligations. TURN and libdatachannel remain outside this provider.



The independent NativeWorkerPackage/v1 header is a separately pinned, unpublished
Dockpipe host SDK review artifact (Apache-2.0, Dockpipe contributors). Its C ABI is
independent of the published Qt DisplaySDK. It is an explicit build input, not a
copied implementation or a published compatibility promise.
