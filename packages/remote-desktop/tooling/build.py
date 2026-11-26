#!/usr/bin/env python3
"""Build an isolated native provider candidate from locked, offline inputs."""
from pathlib import Path
import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import tempfile

import dependencies


PACKAGE = Path(__file__).resolve().parents[1]
MAX_OUTPUT_FILES = 4096
MAX_OUTPUT_BYTES = 128 << 20


def run(arguments, environment=None, timeout=300, capture=False):
    return subprocess.run([str(argument) for argument in arguments], check=True, timeout=timeout,
                          env=environment, text=True, capture_output=capture)


def selected_executable(value, label):
    path = Path(value)
    if not path.is_absolute() or not path.is_file() or not os.access(path, os.X_OK):
        raise ValueError(label + " must select an absolute executable")
    return path


def inventory(root):
    result = []
    total = 0
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError("Native candidate payload must not contain aliases")
        if not path.is_file():
            continue
        total += path.stat().st_size
        if len(result) >= MAX_OUTPUT_FILES or total > MAX_OUTPUT_BYTES:
            raise ValueError("Native candidate exceeds its 4096-file/128-MiB bound")
        result.append({"path": path.relative_to(root).as_posix(), "bytes": path.stat().st_size,
                       "sha256": dependencies.digest(path)})
    return result


def verify_source():
    path = PACKAGE / "dependencies/source-files.lock.json"
    if not path.is_file():
        raise ValueError("Component owners have not frozen the native provider source inventory")
    lock = json.loads(path.read_text())
    if lock.get("status") != "owner-frozen":
        raise ValueError("Native provider source freeze is incomplete; await the owning component files")
    expected = set()
    for record in lock["files"]:
        relative = Path(record["path"])
        if relative.is_absolute() or ".." in relative.parts or relative.parts[0] not in ("source", "native", "channel", "tests"):
            raise ValueError("Invalid frozen native source path")
        dependencies.verify_file(PACKAGE / relative, record)
        expected.add(relative.as_posix())
    actual = {path.relative_to(PACKAGE).as_posix()
              for name in ("source", "native", "channel", "tests")
              for path in (PACKAGE / name).rglob("*")
              if path.is_file() and "__pycache__" not in path.parts
              and (name != "tests" or path.suffix in (".cpp", ".h") or path.name == "viewer_fixture.py" or "fixtures" in path.parts)}
    if actual != expected:
        raise ValueError("Native source files differ from the owner-frozen inventory")
    return lock


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def bind_resolver_platform(resolver, platform_name):
    manifest = resolver / "package.yml"
    declaration = "platforms: [deb, macos]"
    text = manifest.read_text()
    if text.count(declaration) != 1:
        raise ValueError("Resolver platform declaration differs from the source contract")
    manifest.write_text(text.replace(declaration, f"platforms: [{platform_name}]"))


def copy_licenses(prepared, destination, go_root):
    destination.mkdir(parents=True)
    for relative, name in [("sdk/share/DockPipeDisplaySDK/LICENSE", "Dockpipe-DisplaySDK-Apache-2.0.txt"),
                           ("json/LICENSE.MIT", "nlohmann-json-MIT.txt")]:
        shutil.copyfile(prepared / relative, destination / name)
    for path in sorted((prepared / "ffmpeg").glob("COPYING*")):
        shutil.copyfile(path, destination / ("FFmpeg-" + path.name))
    ffmpeg_notice = prepared / "ffmpeg/LICENSE.md"
    if ffmpeg_notice.is_file():
        shutil.copyfile(ffmpeg_notice, destination / "FFmpeg-LICENSE.md")
    for name in ("LICENSE", "PATENTS"):
        source = go_root / name
        if not source.is_file():
            raise ValueError("Selected Go toolchain is missing its " + name + " notice")
        shutil.copyfile(source, destination / ("Go-" + name))
    shutil.copyfile(PACKAGE / "LICENSE", destination / "Dockpipe-remote-desktop-Apache-2.0.txt")
    shutil.copyfile(PACKAGE / "THIRD-PARTY-NOTICES.md", destination / "THIRD-PARTY-NOTICES.md")
    cuda_declarations = PACKAGE / "source/cuda/dynlink_cuda.h"
    shutil.copyfile(cuda_declarations, destination / "nv-codec-headers-MIT.h")


def build(args):
    system = platform.system()
    machine = platform.machine()
    if system == "Linux" and machine in ("x86_64", "amd64"):
        platform_name = "linux-amd64"
        go_system, go_arch = "linux", "amd64"
    elif system == "Darwin" and machine == "arm64":
        platform_name = "darwin-arm64"
        go_system, go_arch = "darwin", "arm64"
    else:
        raise ValueError("Native production supports only Linux amd64 viewer and Darwin arm64 sender")
    output = Path(args.output)
    inputs = Path(args.inputs)
    if not output.is_absolute() or output.exists():
        raise ValueError("--output must be a fresh absolute candidate directory")
    source = verify_source()
    go = selected_executable(args.go, "--go")
    cmake = selected_executable(args.cmake, "--cmake")
    environment = dict(os.environ)
    environment.update(GOTOOLCHAIN="local", GOPROXY="off", GOSUMDB="off", CGO_ENABLED="0",
                       GOOS=go_system, GOARCH=go_arch)
    version = run([go, "version"], environment, timeout=10, capture=True).stdout.strip()
    if not re.fullmatch(r"go version go1\.26\.10 (linux/amd64|darwin/arm64)", version):
        raise ValueError("The producer requires explicit Go 1.26.10 for the native platform")
    go_root = Path(run([go, "env", "GOROOT"], environment, timeout=10, capture=True).stdout.strip())
    lock = dependencies.lockfile()
    qt = Path(args.qt_prefix) if args.qt_prefix else None
    host_libraries = []
    host_prefix = Path(args.host_library_prefix) if args.host_library_prefix else None
    if platform_name == "linux-amd64":
        if qt is None:
            raise ValueError("Linux viewer production requires --qt-prefix")
        dependencies.verify_qt_runtime(qt, lock)
        if host_prefix is None:
            raise ValueError("Linux viewer production requires --host-library-prefix")
        host_libraries = dependencies.verify_host_runtime(host_prefix, lock)
    elif not args.signing_identity:
        raise ValueError("Darwin production requires explicit --signing-identity ('-' is review-only ad-hoc)")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-desktop-build-", dir=output.parent) as temporary:
        task = Path(temporary)
        prepared = dependencies.prepare(inputs, task / "dependencies", platform_name)
        build_directory = task / "native"
        resolver = task / "resolver"
        shutil.copytree(PACKAGE / "resolvers/dockpipe.desktop.native", resolver)
        bind_resolver_platform(resolver, "deb" if platform_name == "linux-amd64" else "macos")
        if platform_name == "linux-amd64":
            dependencies.copy_linux_media_sources(inputs, resolver / "assets/media-sources", lock)
        configure = [cmake, "-S", PACKAGE, "-B", build_directory, "-DCMAKE_BUILD_TYPE=Release",
                     "-DDOCKPIPE_DESKTOP_DEPENDENCIES=" + str(prepared)]
        if qt:
            configure.extend(["-DCMAKE_PREFIX_PATH=" + str(qt), "-DDOCKPIPE_DESKTOP_QT_PREFIX=" + str(qt),
                              "-DDOCKPIPE_DESKTOP_HOST_LIBRARY_PREFIX=" + str(host_prefix),
                              "-DPython3_EXECUTABLE=" + str(Path(os.sys.executable).resolve())])
        run(configure)
        run([cmake, "--build", build_directory, "--parallel", str(args.jobs)])
        test_cache = task / "test-cache"
        test_temporary = task / "test-tmp"
        test_cache.mkdir(mode=0o700)
        test_temporary.mkdir(mode=0o700)
        test_environment = dict(os.environ, XDG_CACHE_HOME=str(test_cache), TMPDIR=str(test_temporary))
        run(["ctest", "--test-dir", build_directory, "--output-on-failure"], test_environment, timeout=90)
        run([cmake, "--install", build_directory, "--prefix", resolver])
        channel = resolver / "assets/tooling/bin/dockpipe-desktop-channel"
        channel.parent.mkdir(parents=True, exist_ok=True)
        environment.update(GOCACHE=str(task / "go-cache"), GOTMPDIR=str(task / "go-tmp"))
        Path(environment["GOTMPDIR"]).mkdir()
        run([go, "-C", PACKAGE / "channel", "test", "-count=1", "./..."], environment, timeout=120)
        run([go, "-C", PACKAGE / "channel", "build", "-trimpath", "-buildvcs=false", "-o", channel, "."], environment)
        if platform_name == "linux-amd64":
            display = resolver / "assets/display"
            runtime = display / "lib"
            runtime.mkdir()
            for record in lock["ffmpeg"]["linux-amd64_runtime"]:
                copied = runtime / record["soname"]
                shutil.copyfile(qt / "lib" / record["file"], copied)
                dependencies.verify_file(copied, record)
            module = display / "libdockpipe-native-desktop-display.so"
            metadata = json.loads((build_directory / "display-metadata.json").read_text())
            write_json(display / "display-factory.json", {
                "schema": 1, "interface": lock["host_sdk"]["interface"], "platform": platform_name,
                "library": module.name, "sha256": dependencies.digest(module),
                "size_bytes": module.stat().st_size,
                "channel_helper": {"path": "assets/tooling/bin/dockpipe-desktop-channel",
                                   "sha256": dependencies.digest(channel)}, **metadata})
        else:
            surface_path = resolver / "assets/ui/surface.json"
            surface = json.loads(surface_path.read_text())
            surface.pop("native_display", None)
            write_json(surface_path, surface)
            bundle = resolver / "assets/tooling/dockpipe-native-desktop-worker.app"
            # Sign children first, then the application. The runtime manifest is
            # outside the sealed bundle and binds final signed executable bytes.
            run(["codesign", "--force", "--sign", args.signing_identity, channel], timeout=30)
            run(["codesign", "--force", "--deep", "--sign", args.signing_identity, bundle], timeout=30)
            run(["codesign", "--verify", "--deep", "--strict", bundle], timeout=30)
            worker_module = resolver / "assets/worker/libdockpipe-native-desktop-worker-module.so"
            run(["codesign", "--force", "--sign", args.signing_identity, worker_module], timeout=30)
            run(["codesign", "--verify", "--strict", worker_module], timeout=30)
            worker = bundle / "Contents/MacOS/dockpipe-native-desktop-worker"
            manifest = resolver / "assets/tooling/provider-runtime.json"
            write_json(manifest, {"schema": "dockpipe.desktop.native.provider/v1",
                                 "provider": {"id": "dockpipe.desktop.native", "version": "0.1.0",
                                              "artifact": dependencies.digest(worker)},
                                 "channel_helper_sha256": dependencies.digest(channel)})
            manifest.chmod(0o600)
            module_manifest = resolver / "assets/tooling/provider-module-runtime.json"
            write_json(module_manifest, {"schema": "dockpipe.desktop.native.provider/v1",
                                        "provider": {"id": "dockpipe.desktop.native", "version": "0.1.0",
                                                     "artifact": dependencies.digest(worker_module)},
                                        "channel_helper_sha256": dependencies.digest(channel)})
            module_manifest.chmod(0o600)
            write_json(resolver / "assets/worker/worker-factory.json", {
                "schema": "dockpipe.desktop.native.worker/v1",
                "interface": "dockpipe.native-worker-sdk/v1", "abi_version": 1,
                "entrypoint": "dockpipe_native_worker_package_v1",
                "sdk_header_sha256": lock["native_worker_sdk"]["sha256"],
                "module": {"path": worker_module.relative_to(resolver).as_posix(),
                           "sha256": dependencies.digest(worker_module)},
                "manifest": module_manifest.relative_to(resolver).as_posix(),
                "channel_helper": {"path": channel.relative_to(resolver).as_posix(),
                                   "sha256": dependencies.digest(channel)}})
        if platform_name == "linux-amd64":
            run([os.sys.executable, PACKAGE / "tooling/verify_linux_runtime.py", "--root", resolver,
                 "--qt-prefix", qt, "--receipt", resolver / "assets/runtime-load.json"], timeout=20)
        copy_licenses(prepared, resolver / "assets/licenses", go_root)
        signing = "not-applicable"
        if platform_name == "darwin-arm64":
            signing = "ad-hoc-review-only" if args.signing_identity == "-" else "operator-signature"
        write_json(resolver / "assets/provider.json", {
            "schema": "dockpipe.desktop.native.artifact/v1", "id": "dockpipe.desktop.native", "version": "0.1.0",
            "platform": platform_name, "role": "viewer" if platform_name == "linux-amd64" else "sender",
            "transport": "wss-inner-tls", "tests_run": True,
            "signing": signing,
            "distribution_qualified": False})
        result = task / "candidate"
        result.mkdir()
        shutil.move(resolver, result / "resolver")
        write_json(result / "source-inventory.json", source)
        write_json(result / "dependency-lock.json", lock)
        producer = {"schema": 1, "platform": platform_name,
                   "go": version, "go_sha256": dependencies.digest(go),
                   "cmake": run([cmake, "--version"], timeout=10, capture=True).stdout.splitlines()[0],
                   "tests_run": True, "host_native_libraries": host_libraries,
                   "native_capture_input_qualified": False,
                   "distribution_qualified": False}
        write_json(result / "producer.json", producer)
        provenance = result / "resolver/assets/provenance"
        provenance.mkdir()
        write_json(provenance / "source-inventory.json", source)
        write_json(provenance / "dependency-lock.json", lock)
        write_json(provenance / "producer.json", producer)
        write_json(result / "inventory.json", {"schema": 1, "files": inventory(result)})
        shutil.copytree(result, output)
    print(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    parser.add_argument("--inputs", required=True)
    parser.add_argument("--go", required=True)
    parser.add_argument("--qt-prefix")
    parser.add_argument("--host-library-prefix", default=os.environ.get("DOCKPIPE_DESKTOP_HOST_LIBRARY_PREFIX", ""))
    parser.add_argument("--cmake", default=shutil.which("cmake") or "")
    parser.add_argument("--signing-identity", default=os.environ.get("DOCKPIPE_DESKTOP_SIGNING_IDENTITY", ""))
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1 or args.jobs > 8:
        parser.error("--jobs must be between one and eight")
    try:
        build(args)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit("Native desktop candidate build failed: " + str(error)) from error


if __name__ == "__main__":
    main()
