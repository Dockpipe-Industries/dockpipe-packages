"""Offline, bounded dependency admission for the native desktop package."""
from pathlib import Path, PurePosixPath
import hashlib
import json
import shutil
import tarfile
import tempfile


PACKAGE = Path(__file__).resolve().parents[1]
MAX_ARCHIVE_BYTES = 32 << 20
MAX_SELECTED_BYTES = 32 << 20
MAX_SELECTED_FILES = 2048


def digest(path):
    hasher = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1 << 20), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def lockfile():
    return json.loads((PACKAGE / "dependencies/inputs.lock.json").read_text())


def verify_file(path, record):
    if path.is_symlink() or not path.is_file():
        raise ValueError("Dependency input must be a regular file: " + path.name)
    size = path.stat().st_size
    if size != record["bytes"] or size > MAX_ARCHIVE_BYTES or digest(path) != record["sha256"]:
        raise ValueError("Dependency input size or SHA-256 mismatch: " + path.name)


def archive_files(archive):
    """Reject aliases and ambiguous paths before selecting any dependency files."""
    files = {}
    members = archive.getmembers()
    if len(members) > 30000:
        raise ValueError("Dependency archive exceeds its member bound")
    for member in members:
        path = PurePosixPath(member.name)
        if path.is_absolute() or ".." in path.parts or str(path) != member.name.rstrip("/"):
            raise ValueError("Dependency archive contains an unsafe path")
        if member.isdir():
            continue
        if not member.isfile() or member.name in files:
            raise ValueError("Dependency archive contains aliases or duplicate files")
        files[member.name] = member
    return files


def extract_selected(archive, selected, destination):
    if len(selected) > MAX_SELECTED_FILES or sum(member.size for _, member in selected) > MAX_SELECTED_BYTES:
        raise ValueError("Selected dependency files exceed their extraction bound")
    for relative, member in selected:
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        with archive.extractfile(member) as source, target.open("xb") as output:
            shutil.copyfileobj(source, output, length=1 << 20)


def prepare_sdk(inputs, destination, platform_name, lock):
    records = [record for record in lock["host_sdk"]["artifacts"] if record["platform"] == platform_name]
    if len(records) != 3:
        raise ValueError("No complete pinned DisplaySDK for the selected platform")
    directory = inputs / platform_name
    for record in records:
        verify_file(directory / record["name"], record)
    release = json.loads((directory / "sdk-release.json").read_text())
    expected = {"product": "DockPipeDisplaySDK", "version": "1.0.0", "platform": platform_name,
                "interface": lock["host_sdk"]["interface"], "qt_version": lock["qt"]["version"],
                "source_sha": lock["host_sdk"]["source_commit"]}
    if any(release.get(key) != value for key, value in expected.items()):
        raise ValueError("DisplaySDK release identity mismatch")
    manifest = json.loads((directory / "sdk-files.json").read_text())
    entries = manifest.get("files", [])
    if manifest.get("schema") != 1 or len(entries) != 7:
        raise ValueError("DisplaySDK file inventory does not match the seven-file contract")
    root = "DockPipeDisplaySDK-1.0.0-" + platform_name + "/"
    with tarfile.open(directory / release["archive"]["name"]) as archive:
        files = archive_files(archive)
        expected_paths = {root + entry["path"] for entry in entries}
        if len(expected_paths) != len(entries) or set(files) != expected_paths:
            raise ValueError("DisplaySDK archive differs from its file inventory")
        selected = []
        for entry in entries:
            member = files[root + entry["path"]]
            data = archive.extractfile(member).read((2 << 20) + 1)
            if len(data) != entry["bytes"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
                raise ValueError("DisplaySDK payload digest mismatch")
            selected.append((entry["path"], member))
        extract_selected(archive, selected, destination / "sdk")
    header = destination / "sdk/include/dockpipe-launcher/LauncherDisplayPackage.h"
    if digest(header) != lock["host_sdk"]["header_sha256"]:
        raise ValueError("Display interface header ABI identity mismatch")



def prepare_worker_sdk(inputs, destination, lock):
    record = lock["native_worker_sdk"]
    relative = PurePosixPath(record["path"])
    if (record["schema"] != "dockpipe.native-worker-sdk/v1" or record["abi_version"] != 1
            or record["entrypoint"] != "dockpipe_native_worker_package_v1"
            or relative.is_absolute() or ".." in relative.parts
            or relative.name != "DockpipeNativeWorkerPackage.h"):
        raise ValueError("Unsupported native worker SDK ABI or artifact path")
    header = inputs / record["path"]
    verify_file(header, record)
    include = destination / "worker-sdk/include"
    include.mkdir(parents=True)
    shutil.copyfile(header, include / relative.name)


def prepare_source(inputs, destination, record, source_root, selector):
    path = inputs / record["archive"]
    verify_file(path, record)
    with tarfile.open(path) as archive:
        files = archive_files(archive)
        selected = []
        for name, member in files.items():
            if not name.startswith(source_root + "/"):
                raise ValueError("Dependency archive has an unexpected source root")
            relative = name[len(source_root) + 1:]
            if selector(relative):
                selected.append((relative, member))
        if not selected:
            raise ValueError("Dependency archive lacks the required source inputs")
        extract_selected(archive, selected, destination)


def prepare(inputs, destination, platform_name):
    if platform_name not in ("linux-amd64", "darwin-arm64"):
        raise ValueError("Only Linux amd64 viewer and Darwin arm64 sender are supported")
    if not inputs.is_absolute() or not inputs.is_dir() or not destination.is_absolute() or destination.exists():
        raise ValueError("Dependency preparation requires absolute inputs and a fresh output directory")
    lock = lockfile()
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".desktop-dependencies-", dir=destination.parent) as temporary:
        staging = Path(temporary)
        prepare_sdk(inputs, staging, platform_name, lock)
        prepare_worker_sdk(inputs, staging, lock)
        prepare_source(inputs, staging / "json", lock["json"], "json-" + lock["json"]["commit"],
                       lambda name: name in ("single_include/nlohmann/json.hpp", "LICENSE.MIT"))
        if platform_name == "linux-amd64":
            roots = ("libavcodec/", "libavutil/", "libswscale/", "libswresample/")
            licenses = ("COPYING.LGPLv2.1", "COPYING.LGPLv3", "COPYING.GPLv2", "COPYING.GPLv3", "LICENSE.md", "COPYING.GPLv2")
            prepare_source(inputs, staging / "ffmpeg", lock["ffmpeg"], "ffmpeg-7.1.1",
                           lambda name: name in licenses or name.startswith(roots) and name.endswith(".h"))
            # Public FFmpeg headers require this generated target ABI header.
            # This package supports only little-endian amd64 for the decoder.
            (staging / "ffmpeg/libavutil/avconfig.h").write_text(
                "/* Package-selected FFmpeg 7.1.1 Linux amd64 public header ABI. */\n"
                "#ifndef AVUTIL_AVCONFIG_H\n#define AVUTIL_AVCONFIG_H\n"
                "#define AV_HAVE_BIGENDIAN 0\n#define AV_HAVE_FAST_UNALIGNED 1\n#endif\n")
        shutil.copytree(staging, destination)
    return destination


def copy_linux_media_sources(inputs, destination, lock):
    """Ship the corresponding sources and recipes for the locked media binaries."""
    distribution = lock["ffmpeg"]["linux-amd64_distribution"]
    records = distribution["sources"]
    for record in records:
        verify_file(inputs / record["file"], record)

    destination.mkdir(parents=True)
    for record in records:
        target = destination / record["file"]
        shutil.copyfile(inputs / record["file"], target)
        verify_file(target, record)

    manifest = {
        "schema": 1,
        "ffmpeg_version": distribution["ffmpeg_version"],
        "qt_multimedia_version": distribution["qt_multimedia_version"],
        "sources": records,
    }
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def verify_qt_runtime(qt, lock):
    if not qt.is_absolute() or not qt.is_dir():
        raise ValueError("An explicit Qt 6.10.3 Linux kit is required")
    version_file = qt / "lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake"
    if not version_file.is_file() or 'set(PACKAGE_VERSION "6.10.3")' not in version_file.read_text():
        raise ValueError("The selected Qt kit must be exactly 6.10.3")
    for record in lock["ffmpeg"]["linux-amd64_runtime"]:
        verify_file(qt / "lib" / record["file"], record)
    return lock["ffmpeg"]["linux-amd64_runtime"]


def verify_host_runtime(prefix, lock):
    if not prefix.is_absolute() or not prefix.is_dir():
        raise ValueError("Linux media requires an explicit qualified host-library prefix")
    resolved = prefix.resolve(strict=True)
    records = []
    for record in lock["linux_host_runtime"]["libraries"]:
        selected = (prefix / record["file"]).resolve(strict=True)
        if not selected.is_relative_to(resolved) or selected.name != record["resolved_file"]:
            raise ValueError("Host media library escapes or differs from its pinned selection")
        verify_file(selected, record)
        records.append({**record, "selected_path": str(selected)})
    return records
