#!/usr/bin/env python3
"""Verify the relocated provider resolves its private media closure, offline."""
from pathlib import Path
import argparse
import ctypes
import hashlib
import json
import os
import subprocess
import sys
import tempfile


PACKAGE = Path(__file__).resolve().parents[1]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inspect_loaded(root):
    lock = json.loads((PACKAGE / "dependencies/inputs.lock.json").read_text())
    module = root / "assets/display/libdockpipe-native-desktop-display.so"
    ctypes.CDLL(str(module), mode=os.RTLD_NOW | os.RTLD_LOCAL)
    mappings = Path("/proc/self/maps").read_text().splitlines()
    libraries = []
    for record in lock["ffmpeg"]["linux-amd64_runtime"]:
        path = root / "assets/display/lib" / record["soname"]
        if path.is_symlink() or not any(line.endswith(str(path)) for line in mappings):
            raise ValueError("Media dependency did not resolve from the private candidate: " + record["soname"])
        if sha256(path) != record["sha256"]:
            raise ValueError("Loaded private media dependency differs from its pin")
        libraries.append({"file": record["soname"], "sha256": record["sha256"], "origin": "package-private"})
    avutil = ctypes.CDLL(str(root / "assets/display/lib/libavutil.so.59"), mode=os.RTLD_NOW | os.RTLD_LOCAL)
    avutil.av_version_info.restype = ctypes.c_char_p
    avutil.avutil_configuration.restype = ctypes.c_char_p
    version = avutil.av_version_info().decode("ascii")
    configuration = avutil.avutil_configuration().decode("ascii")
    distribution = lock["ffmpeg"]["linux-amd64_distribution"]
    if version != distribution["ffmpeg_version"]:
        raise ValueError("Loaded FFmpeg version differs from its corresponding source")

    host_libraries = []
    for record in lock["linux_host_runtime"]["libraries"]:
        matches = {Path(line.split()[-1]) for line in mappings if line.endswith("/" + record["resolved_file"])}
        if not matches or any(sha256(path) != record["sha256"] for path in matches):
            raise ValueError("Loaded host media dependency differs from its qualified pin: " + record["file"])
        host_libraries.append({"file": record["file"], "sha256": record["sha256"], "origin": "host"})
    descriptor = json.loads((root / "assets/display/display-factory.json").read_text())
    helper = root / descriptor["channel_helper"]["path"]
    if sha256(helper) != descriptor["channel_helper"]["sha256"]:
        raise ValueError("Packaged channel helper differs from its factory descriptor")
    print(json.dumps({"schema": 1, "result": "pass", "load": "RTLD_NOW|RTLD_LOCAL",
                      "module_sha256": sha256(module), "private_runtime": libraries, "host_runtime": host_libraries,
                      "ffmpeg_version": version, "ffmpeg_configuration": configuration,
                      "channel_helper_sha256": descriptor["channel_helper"]["sha256"],
                      "capture_input_or_network_performed": False}, indent=2))


def verify(root, qt, receipt):
    with tempfile.TemporaryDirectory(prefix="desktop-host-qt-") as temporary:
        host_libraries = Path(temporary)
        # Qt is supplied by the matching host. Exclude every FFmpeg/stub name so
        # LD_LIBRARY_PATH cannot mask a missing private transitive dependency.
        for library in (qt / "lib").iterdir():
            if not library.is_file() or ".so" not in library.name:
                continue
            if library.name.startswith(("libav", "libsw", "libQt6FFmpegStub-")):
                continue
            (host_libraries / library.name).symlink_to(library.resolve(strict=True))
        environment = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8", "LD_LIBRARY_PATH": str(host_libraries),
                       "PYTHONDONTWRITEBYTECODE": "1"}
        result = subprocess.run([sys.executable, str(Path(__file__).resolve()), "--root", str(root), "--inspect"],
                                env=environment, capture_output=True, text=True, check=True, timeout=15)
        evidence = json.loads(result.stdout)
        receipt.write_text(json.dumps(evidence, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True)
    parser.add_argument("--qt-prefix")
    parser.add_argument("--receipt")
    parser.add_argument("--inspect", action="store_true", help=argparse.SUPPRESS)
    arguments = parser.parse_args()
    root = Path(arguments.root)
    if not root.is_absolute() or not root.is_dir() or root.is_symlink():
        parser.error("--root must select an absolute regular candidate directory")
    if arguments.inspect:
        inspect_loaded(root)
        return
    if not arguments.qt_prefix or not arguments.receipt:
        parser.error("--qt-prefix and --receipt are required")
    qt = Path(arguments.qt_prefix)
    receipt = Path(arguments.receipt)
    if not qt.is_absolute() or not qt.is_dir() or not receipt.is_absolute() or receipt.exists():
        parser.error("Select an absolute Qt kit and a fresh absolute receipt")
    verify(root, qt, receipt)


if __name__ == "__main__":
    main()
