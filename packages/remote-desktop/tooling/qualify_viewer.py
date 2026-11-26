#!/usr/bin/env python3
"""Build the Linux desktop viewer and export its staging candidate store."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

import dependencies


PACKAGE = Path(__file__).resolve().parents[1]
CATALOG = PACKAGE.parents[1]
NATIVE_SCRIPTS = CATALOG / "workflows/package/package-native-store-candidate/assets/scripts"
sys.path.insert(0, str(NATIVE_SCRIPTS))
import native_inputs
import native_store
import toolchain_acquire
import toolchain_inputs


def run(arguments, **options):
    return subprocess.run([str(item) for item in arguments], check=True, **options)


def load(path):
    return json.loads(path.read_bytes())


def fetch(record, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    run(["curl", "--fail", "--silent", "--show-error", "--location",
         "--proto", "=https", "--proto-redir", "=https", "--connect-timeout", "15",
         "--max-time", "180", "--max-filesize", record["bytes"],
         "--output", destination, record["url"]], timeout=190)
    dependencies.verify_file(destination, record)


def prepare_inputs(lock, inputs):
    for record in lock["host_sdk"]["artifacts"]:
        if record["platform"] == "linux-amd64":
            fetch(record, inputs / record["platform"] / record["name"])

    for record in (lock["json"], lock["ffmpeg"]):
        fetch(record, inputs / record["archive"])

    for record in lock["ffmpeg"]["linux-amd64_distribution"]["sources"]:
        fetch(record, inputs / record["file"])

    worker = lock["native_worker_sdk"]
    fetch(worker, inputs / worker["path"])


def prepare_qt(profile, multimedia, cache, root):
    entries = {entry["id"]: entry for entry in profile["native_inputs"]["archives"]}
    components = [entries["qt-qtbase"], entries["qt-icu"], multimedia]
    root.mkdir()
    writer = native_inputs.PrefixWriter(root, case_sensitive=True)

    for component in components:
        archive = native_store.acquire(component["asset"], cache, fetch=True, upstream=True)
        native_inputs.sevenzip_members(component, archive, writer)

    writer.finish_aliases()
    return root / "qt/6.10.3/gcc_64"


def compile_store(cli, candidate, root, store, environment):
    workspace = root / "workspace"
    source = workspace / "resolvers" / "dockpipe.desktop.native"
    source.parent.mkdir(parents=True)
    shutil.copytree(candidate / "resolver", source)

    run([cli, "package", "compile", "resolvers", "--workdir", workspace,
         "--from", source.parent], env=environment, timeout=120)
    run([cli, "package", "build", "store", "--only", "resolvers", "--workdir",
         workspace, "--out", store], env=environment, timeout=120)

    manifest = load(store / "packages-store-manifest.json")
    entry, = manifest["packages"]["resolvers"]
    if (manifest["platform"] != "linux-amd64" or entry["name"] != "dockpipe.desktop.native"
            or entry["version"] != "0.1.0"
            or entry["tarball"] != "dockpipe-resolver-dockpipe.desktop.native-0.1.0.tar.gz"
            or not 0 < entry["size_bytes"] <= 128 << 20):
        raise ValueError("Unexpected viewer store identity")

    archive = store / entry["tarball"]
    if archive.stat().st_size != entry["size_bytes"] or dependencies.digest(archive) != entry["sha256"]:
        raise ValueError("Viewer store archive differs from its compiled manifest")


def qualify(root):
    root.mkdir(mode=0o700)
    cache = root / "cache"
    cache.mkdir()
    lock = dependencies.lockfile()
    release = load(PACKAGE / "dependencies/linux-viewer-candidate.lock.json")
    profile = load(CATALOG / "release/native-store.lock.json")["profiles"]["linux-amd64"]

    inputs = root / "inputs"
    prepare_inputs(lock, inputs)
    qt = prepare_qt(profile, release["multimedia"], cache, root / "qt-inputs")
    toolchain = load(CATALOG / "release/native-toolchain/linux-amd64.json")
    go, = [item for item in toolchain["inputs"] if item["id"] == "go"]
    if "go" + go["version"] != lock["go"]["version"]:
        raise ValueError("Viewer and catalog Go versions differ")
    toolchain_inputs.unpack(go, toolchain_acquire.acquire(go, cache), root / "go")
    cli_pin = release["cli"]
    cli_root = native_store.extract(cli_pin, native_store.acquire(cli_pin, cache, fetch=True), root / "cli")
    cli = cli_root / "dockpipe"
    if run([cli, "--version"], capture_output=True, text=True, timeout=15).stdout.strip() != release["cli_version"]:
        raise ValueError("Viewer packaging CLI does not match its standalone version pin")

    for directory in ("home", "tmp", "go-cache"):
        (root / directory).mkdir()
    candidate = root / "candidate"
    environment = dict(os.environ, HOME=str(root / "home"), TMPDIR=str(root / "tmp"),
                       GOCACHE=str(root / "go-cache"), DOCKPIPE_GLOBAL_ROOT=str(root / "user-store"),
                       DOCKPIPE_DESKTOP_DEPENDENCY_INPUTS=str(inputs),
                       DOCKPIPE_DESKTOP_GO=str(root / "go/bin/go"),
                       DOCKPIPE_DESKTOP_QT_PREFIX=str(qt),
                       DOCKPIPE_DESKTOP_HOST_LIBRARY_PREFIX="/usr/lib/x86_64-linux-gnu",
                       DOCKPIPE_DESKTOP_OUTPUT=str(candidate),
                       DOCKPIPE_TEST_INPUT_NATIVE_X11="1", QT_QPA_PLATFORM="xcb")
    build_workspace = root / "build-workspace"
    package_source = build_workspace / "packages/remote-desktop"
    shutil.copytree(PACKAGE, package_source, ignore=shutil.ignore_patterns("__pycache__"))
    run(["xvfb-run", "-a", cli, "--no-compile-deps", "--no-vault", "--no-op-inject",
         "--workdir", build_workspace, "--workflow-file",
         package_source / "workflows/build/config.yml"], env=environment, timeout=1200)

    store = root / "staging-candidate"
    compile_store(cli, candidate, root, store, environment)
    for name in ("producer.json", "inventory.json", "dependency-lock.json"):
        shutil.copyfile(candidate / name, store / name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    qualify(args.work.resolve())
