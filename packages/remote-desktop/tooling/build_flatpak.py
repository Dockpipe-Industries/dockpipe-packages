#!/usr/bin/env python3
"""Produce and qualify a private KDE 6.10 viewer candidate in explicit stages."""
import argparse
import configparser
import ctypes
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile

import build as native_build
import dependencies


PACKAGE = Path(__file__).resolve().parents[1]
PROFILE = json.loads((PACKAGE / "dependencies/flatpak.lock.json").read_text())
SDK_PATHS = {
    "include/dockpipe-launcher/LauncherDisplayPackage.h",
    "include/dockpipe-launcher/LauncherNativeLibraryScope.h",
    "lib/cmake/DockPipeDisplaySDK/DockPipeDisplaySDKConfig.cmake",
    "lib/cmake/DockPipeDisplaySDK/DockPipeDisplaySDKConfigVersion.cmake",
    "lib/cmake/DockPipeDisplaySDK/DockPipeDisplaySDKTargets.cmake",
    "share/DockPipeDisplaySDK/LICENSE",
    "share/DockPipeDisplaySDK/display-sdk-candidate.json",
}


def read_json(path, maximum=65536):
    if path.is_symlink() or not path.is_file() or path.stat().st_size > maximum:
        raise ValueError("Expected a bounded regular JSON input: " + str(path))
    return json.loads(path.read_text())


def require_runtime(role):
    info = configparser.ConfigParser()
    info.read("/.flatpak-info")
    expected = PROFILE[role]
    if (platform.machine() != "x86_64" or not info.has_section("Runtime")
            or info.get("Runtime", "runtime", fallback="") != expected["ref"]
            or info.get("Instance", "runtime-commit", fallback="") != expected["commit"]):
        raise ValueError("Run this stage inside the exact pinned " + expected["ref"])
    if (os.environ.get("QT_QPA_PLATFORM") != "xcb"
            or os.environ.get("DOCKPIPE_CLIPBOARD_OWNED_XVFB") != "1"):
        raise ValueError("Fixture qualification requires its explicitly owned authenticated X11 display")


def viewer_profile(text):
    incoming = ("DOCKPIPE_REMOTE_DESKTOP_WORKER=", "DOCKPIPE_REMOTE_DESKTOP_NATIVE_WORKER=")
    for declaration in incoming:
        if sum(line.startswith(declaration) for line in text.splitlines()) != 1:
            raise ValueError("Native source profile lacks a unique expected worker declaration")
    return "\n".join(line for line in text.splitlines() if not line.startswith(incoming)) + "\n"


def prepare_display_sdk(directory, release_sha256, destination, allow_development):
    if not directory.is_absolute() or directory.is_symlink() or not directory.is_dir():
        raise ValueError("Select an absolute regular DisplaySDK artifact directory")
    release_path = directory / "sdk-release.json"
    release = read_json(release_path)
    if (not re.fullmatch(r"[0-9a-f]{64}", release_sha256)
            or dependencies.digest(release_path) != release_sha256):
        raise ValueError("The selected DisplaySDK release digest differs")
    expected = {"schema": 1, "product": "DockPipeDisplaySDK", "version": "1.0.0",
                "platform": "linux-amd64", "qt_version": PROFILE["qt_version"],
                "interface": PROFILE["interface"], "compiler": PROFILE["compiler"]}
    if any(release.get(key) != value for key, value in expected.items()):
        raise ValueError("DisplaySDK does not match the separate Flatpak ABI profile")
    if release.get("source_state") != "committed-snapshot" and not allow_development:
        raise ValueError("Development SDK requires explicit --allow-development-sdk")
    if release.get("source_state") not in ("committed-snapshot", "development"):
        raise ValueError("Unknown DisplaySDK source provenance")
    required_checks = {"sdk-component-install", "relocated-external-consumer-compile-link", "native-consumer-run"}
    if not required_checks.issubset(release.get("checks", [])):
        raise ValueError("DisplaySDK lacks its owning producer consumer checks")
    manifest_record = release.get("file_manifest", {})
    manifest = read_json(directory / "sdk-files.json")
    if (manifest_record.get("name") != "sdk-files.json"
            or dependencies.digest(directory / "sdk-files.json") != manifest_record.get("sha256")):
        raise ValueError("DisplaySDK file manifest differs from its release")
    entries = manifest.get("files", [])
    if manifest.get("schema") != 1 or len(entries) != 7 or {row["path"] for row in entries} != SDK_PATHS:
        raise ValueError("DisplaySDK must contain the exact seven-file interface closure")
    archive_record = release.get("archive", {})
    archive_name = "DockPipeDisplaySDK-1.0.0-linux-amd64.tar.gz"
    if archive_record.get("name") != archive_name:
        raise ValueError("Unexpected DisplaySDK archive name")
    archive_path = directory / archive_name
    dependencies.verify_file(archive_path, {"bytes": archive_record["size_bytes"],
                                           "sha256": archive_record["sha256"]})
    prefix = "DockPipeDisplaySDK-1.0.0-linux-amd64/"
    with tarfile.open(archive_path) as archive:
        files = dependencies.archive_files(archive)
        if set(files) != {prefix + row["path"] for row in entries}:
            raise ValueError("DisplaySDK archive differs from its exact inventory")
        selected = []
        for row in entries:
            member = files[prefix + row["path"]]
            if member.size != row["bytes"] or member.size > 2 << 20:
                raise ValueError("DisplaySDK payload exceeds its declared size")
            selected.append((row["path"], member))
        dependencies.extract_selected(archive, selected, destination)
    for row in entries:
        dependencies.verify_file(destination / row["path"], row)
    candidate = read_json(destination / "share/DockPipeDisplaySDK/display-sdk-candidate.json")
    for key in ("interface", "qt_version", "interface_sha256", "native_library_scope_sha256"):
        if candidate.get(key) != PROFILE[key]:
            raise ValueError("Flatpak DisplaySDK interface identity differs: " + key)
    for filename, key in (("LauncherDisplayPackage.h", "interface_sha256"),
                          ("LauncherNativeLibraryScope.h", "native_library_scope_sha256")):
        if dependencies.digest(destination / "include/dockpipe-launcher" / filename) != PROFILE[key]:
            raise ValueError("Flatpak DisplaySDK public header digest differs")
    return release


def build(args):
    require_runtime("sdk")
    output = Path(args.output)
    if not output.is_absolute() or output.exists() or output.is_relative_to(PACKAGE):
        raise ValueError("--output must select a fresh absolute task directory")
    source = native_build.verify_source()
    go = native_build.selected_executable(args.go, "--go")
    output.mkdir(parents=True)
    prepared = dependencies.prepare(Path(args.inputs), output / "dependencies", "linux-amd64")
    # The immutable native SDK was checked above; retain it separately. Never
    # rewrite its compiler contract or import it with the Flatpak compiler.
    (prepared / "sdk").rename(prepared / "native-sdk-reference")
    sdk_release = prepare_display_sdk(Path(args.display_sdk), args.display_sdk_release_sha256,
                                      prepared / "sdk", args.allow_development_sdk)
    build_directory = output / "build"
    resolver = output / "resolver"
    shutil.copytree(PACKAGE / "resolvers/dockpipe.desktop.native", resolver)
    native_build.run(["/usr/bin/cmake", "-S", PACKAGE, "-B", build_directory, "-DCMAKE_BUILD_TYPE=Release",
                      "-DDOCKPIPE_DESKTOP_DEPENDENCIES=" + str(prepared),
                      "-DDOCKPIPE_DESKTOP_RUNTIME_PROFILE=flatpak-kde-6.10", "-DDOCKPIPE_DESKTOP_QT_PREFIX=/usr"])
    native_build.run(["/usr/bin/cmake", "--build", build_directory, "--parallel", str(args.jobs)], timeout=900)
    native_build.run(["/usr/bin/ctest", "--test-dir", build_directory, "--output-on-failure"], timeout=120)
    plan = native_build.run(["/usr/bin/ctest", "--test-dir", build_directory, "--show-only=json-v1"], capture=True)
    native_build.write_json(output / "test-plan.json", json.loads(plan.stdout))
    native_build.run(["/usr/bin/cmake", "--install", build_directory, "--prefix", resolver])
    environment = dict(os.environ, GOTOOLCHAIN="local", GOPROXY="off", GOSUMDB="off", CGO_ENABLED="0",
                       GOOS="linux", GOARCH="amd64", GOCACHE=str(output / "go-cache"), GOTMPDIR=str(output / "go-tmp"))
    Path(environment["GOTMPDIR"]).mkdir()
    version = native_build.run([go, "version"], environment, capture=True).stdout.strip()
    if version != "go version go1.26.10 linux/amd64":
        raise ValueError("The Flatpak viewer requires explicit Go 1.26.10 linux/amd64")
    channel = resolver / "assets/tooling/bin/dockpipe-desktop-channel"
    channel.parent.mkdir(parents=True, exist_ok=True)
    native_build.run([go, "-C", PACKAGE / "channel", "test", "-count=1", "./..."], environment, timeout=120)
    native_build.run([go, "-C", PACKAGE / "channel", "build", "-trimpath", "-buildvcs=false", "-o", channel, "."], environment)
    metadata = read_json(build_directory / "display-metadata.json")
    module = resolver / "assets/display/libdockpipe-native-desktop-display.so"
    native_build.write_json(module.parent / "display-factory.json", {
        "schema": 1, "interface": PROFILE["interface"], "platform": "linux-amd64",
        "library": module.name, "sha256": dependencies.digest(module), "size_bytes": module.stat().st_size,
        "channel_helper": {"path": channel.relative_to(resolver).as_posix(), "sha256": dependencies.digest(channel)},
        **metadata})
    go_root = Path(native_build.run([go, "env", "GOROOT"], environment, capture=True).stdout.strip())
    native_build.copy_licenses(prepared, resolver / "assets/licenses", go_root)
    if native_build.verify_source() != source:
        raise ValueError("Provider source changed during Flatpak production")
    native_build.write_json(output / "source-inventory.json", source)
    native_build.write_json(output / "producer.json", {
        "schema": 1, "profile": PROFILE, "display_sdk": sdk_release,
        "display_sdk_release_sha256": args.display_sdk_release_sha256,
        "go": version, "go_sha256": dependencies.digest(go), "sdk_tests_run": True,
        "platform_tests_run": False, "live_capture_input_qualified": False, "distribution_qualified": False})
    print(output)


def qualify(args):
    require_runtime("runtime")
    root = Path(args.output)
    if not root.is_absolute() or root.is_symlink() or not root.is_dir():
        raise ValueError("--output must select the candidate's absolute task directory")
    producer = read_json(root / "producer.json")
    if producer.get("profile") != PROFILE:
        raise ValueError("Candidate was built against another Flatpak profile")
    plan = read_json(root / "test-plan.json", 1 << 20)
    tests = plan.get("tests", [])
    if not 1 <= len(tests) <= 64:
        raise ValueError("Candidate test plan is empty or exceeds its 64-test bound")
    for test in tests:
        command = test.get("command", [])
        if (not command or any(not isinstance(argument, str) for argument in command)
                or not Path(command[0]).is_absolute()):
            raise ValueError("Candidate test plan has an invalid command")
        environment = dict(os.environ)
        native_build.run(command, environment, timeout=90)
    resolver = root / "resolver"
    descriptor = read_json(resolver / "assets/display/display-factory.json")
    module = resolver / "assets/display/libdockpipe-native-desktop-display.so"
    if dependencies.digest(module) != descriptor.get("sha256"):
        raise ValueError("Candidate module changed after production")
    helper = resolver / "assets/tooling/bin/dockpipe-desktop-channel"
    if dependencies.digest(helper) != descriptor.get("channel_helper", {}).get("sha256"):
        raise ValueError("Candidate channel helper changed after production")
    ctypes.CDLL(str(module), mode=os.RTLD_NOW | os.RTLD_LOCAL)
    mappings = Path("/proc/self/maps").read_text().splitlines()
    names = ("libQt6", "libavcodec", "libavutil", "libswscale", "libstdc++", "libcrypto", "libva")
    libraries = sorted({Path(row.split()[-1]) for row in mappings if "/" in row
                        and Path(row.split()[-1]).name.startswith(names)})
    required = ("libQt6Core", "libQt6Widgets", "libavcodec", "libavutil", "libswscale", "libstdc++")
    if (any(not any(path.name.startswith(name) for path in libraries) for name in required)
            or any(not str(path).startswith("/usr/lib/") for path in libraries)):
        raise ValueError("Viewer runtime libraries did not resolve exclusively from the Platform")
    receipt = {"schema": 1, "profile": PROFILE["profile"], "result": "pass",
               "tests_run": len(tests), "load": "RTLD_NOW|RTLD_LOCAL",
               "libraries": [{"path": str(path), "sha256": dependencies.digest(path)} for path in libraries],
               "live_capture_input_qualified": False, "installed_gui_qualified": False,
               "distribution_qualified": False}
    native_build.write_json(resolver / "assets/runtime-load.json", receipt)
    profile = resolver / "profile"
    profile.write_text(viewer_profile(profile.read_text()))
    manifest = resolver / "package.yml"
    declaration = "platforms: [deb, macos]"
    text = manifest.read_text()
    if text.count(declaration) != 1:
        raise ValueError("Resolver platform declaration differs from the source contract")
    manifest.write_text(text.replace(declaration, "platforms: [flatpak]"))
    native_build.write_json(resolver / "assets/provider.json", {
        "schema": "dockpipe.desktop.native.artifact/v1", "id": "dockpipe.desktop.native", "version": "0.1.0",
        "platform": PROFILE["profile"], "role": "viewer", "transport": "wss-inner-tls",
        "tests_run": True, "distribution_qualified": False})
    producer["platform_tests_run"] = True
    native_build.write_json(root / "producer.json", producer)
    provenance = resolver / "assets/provenance"
    provenance.mkdir()
    for name in ("producer.json", "source-inventory.json"):
        shutil.copyfile(root / name, provenance / name)
    native_build.write_json(provenance / "flatpak-profile.json", PROFILE)
    native_build.write_json(root / "inventory.json", {"schema": 1, "files": native_build.inventory(resolver)})
    print(json.dumps(receipt, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("build", "qualify"))
    parser.add_argument("--output", required=True)
    parser.add_argument("--inputs")
    parser.add_argument("--go")
    parser.add_argument("--display-sdk")
    parser.add_argument("--display-sdk-release-sha256")
    parser.add_argument("--allow-development-sdk", action="store_true")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 8:
        parser.error("--jobs must be within 1..8")
    if args.stage == "build" and any(not getattr(args, key) for key in
                                     ("inputs", "go", "display_sdk", "display_sdk_release_sha256")):
        parser.error("build requires --inputs, --go, --display-sdk and --display-sdk-release-sha256")
    try:
        if args.stage == "build":
            build(args)
        else:
            qualify(args)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(2, "Flatpak viewer candidate failed: " + str(error) + "\n")


if __name__ == "__main__":
    main()
