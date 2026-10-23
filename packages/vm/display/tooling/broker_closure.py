"""Hash-verified private broker input; never select a system service implicitly."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
from urllib.parse import urlsplit

from tooling.runtime_closure import windows_closure, macos_closure


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def broker_inputs():
    names = ("DOCKPIPE_VM_DBUS_DAEMON_BIN", "DOCKPIPE_VM_DBUS_PROVENANCE")
    paths = []
    for name in names:
        value = os.environ.get(name)
        if not value or not Path(value).is_absolute() or not Path(value).is_file():
            raise ValueError(name + " must select an explicit absolute file")
        paths.append(Path(value).resolve())
    binary, descriptor = paths
    if binary.stat().st_size > 64 * 1024 * 1024:
        raise ValueError("Private broker binary exceeds 64 MiB")
    if descriptor.stat().st_size > 65536:
        raise ValueError("Private broker provenance exceeds 64 KiB")
    record = json.loads(descriptor.read_text())
    required = {"schema", "version", "binary_sha256", "source_url", "source_sha256", "licenses"}
    if not isinstance(record, dict) or not required.issubset(record) or \
            set(record) - required - {"patch_sha256", "runtime_libraries"} or record["schema"] != 1:
        raise ValueError("Unsupported private broker provenance schema")
    for field in ("binary_sha256", "source_sha256", "patch_sha256"):
        if field in record and not isinstance(record[field], str):
            raise ValueError("Invalid private broker provenance digest")
        if field in record and not re.fullmatch(r"[0-9a-f]{64}", record[field]):
            raise ValueError("Invalid private broker provenance digest")
    if digest(binary) != record["binary_sha256"]:
        raise ValueError("Private broker binary differs from its exact provenance")
    if not isinstance(record["version"], str) or not re.fullmatch(r"[0-9]+(?:\.[0-9]+){1,3}", record["version"]):
        raise ValueError("Invalid private broker version")
    if not isinstance(record["source_url"], str) or len(record["source_url"]) > 2048:
        raise ValueError("Invalid private broker source URL")
    source = urlsplit(record["source_url"])
    if source.scheme != "https" or not source.hostname or source.username or source.password:
        raise ValueError("Private broker source must be a public HTTPS URL")
    licenses = record["licenses"]
    if not isinstance(licenses, list) or not 1 <= len(licenses) <= 32:
        raise ValueError("Private broker requires a bounded license inventory")
    total = 0
    for entry in licenses:
        if not isinstance(entry, dict) or set(entry) != {"path", "sha256"}:
            raise ValueError("Invalid private broker license entry")
        if not isinstance(entry["path"], str) or not isinstance(entry["sha256"], str) or \
                not re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]):
            raise ValueError("Invalid private broker license entry")
        path = Path(entry["path"])
        if path.is_file() and path.stat().st_size > 16 * 1024 * 1024:
            raise ValueError("Private broker license exceeds 16 MiB")
        if not path.is_absolute() or not path.is_file() or digest(path) != entry["sha256"]:
            raise ValueError("Private broker license differs from its exact provenance")
        total += path.stat().st_size
    if total > 16 * 1024 * 1024:
        raise ValueError("Private broker licenses exceed 16 MiB")
    libraries = record.get("runtime_libraries", [])
    if not isinstance(libraries, list) or len(libraries) > 32:
        raise ValueError("Private broker runtime inventory exceeds its bound")
    names = set()
    for entry in libraries:
        if not isinstance(entry, dict) or set(entry) != {"path", "sha256"}:
            raise ValueError("Invalid private broker runtime entry")
        path = Path(entry["path"])
        if not path.is_absolute() or not path.is_file() or path.stat().st_size > 64 * 1024 * 1024:
            raise ValueError("Invalid private broker runtime file")
        if path.name in names or digest(path) != entry["sha256"]:
            raise ValueError("Private broker runtime differs from exact provenance")
        names.add(path.name)
    return binary, record


def bundle_broker(binary, record, destination, system, gio, qt, inspector,
                  brew, elf_dependencies, package_license):
    runtime = destination / "tooling/bin"
    runtime.mkdir(parents=True)
    target = runtime / ("dbus-daemon.exe" if system == "Windows" else "dbus-daemon")
    shutil.copy2(binary, target)
    admitted = {Path(entry["path"]).name: Path(entry["path"]).resolve()
                for entry in record.get("runtime_libraries", [])}
    if system == "Windows":
        dependencies = windows_closure(target, runtime, gio, qt, inspector)
    elif system == "Darwin":
        dependencies = macos_closure(target, runtime, gio, brew, admitted)
    else:
        libraries = elf_dependencies(binary)
        dependencies = []
        for name, source in sorted(libraries.items()):
            shutil.copyfile(source, runtime / name)
            if source.resolve() in admitted.values():
                package = "private-dbus"
                version = record["version"]
                license_source = Path(record["licenses"][0]["path"])
            else:
                package, version, license_source = package_license(source)
            license_target = destination / "licenses/broker-runtime" / (package + ".copyright")
            license_target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(license_source, license_target)
            dependencies.append({"name": name, "sha256": digest(runtime / name),
                                 "package": package, "version": version})
        # ELF DT_NEEDED lookup does not search the executable directory. A
        # wrapper supplies this private closure without changing the host PATH.
        executable = runtime / "dbus-daemon.real"
        target.rename(executable)
        target.write_text('#!/bin/sh\nset -eu\n'
                          'broker_dir=$(CDPATH= cd "${0%/*}" && pwd)\n'
                          'export LD_LIBRARY_PATH="$broker_dir"\n'
                          'exec "$broker_dir/dbus-daemon.real" "$@"\n')
        target.chmod(0o755)
    public = {key: value for key, value in record.items() if key not in {"licenses", "runtime_libraries"}}
    public["runtime_libraries"] = [{"name": name, "sha256": digest(path)}
                                    for name, path in sorted(admitted.items())]
    public["installed_sha256"] = digest(target)
    public["runtime_dependencies"] = dependencies
    public["licenses"] = []
    for index, entry in enumerate(record["licenses"]):
        source = Path(entry["path"])
        relative = Path("licenses/broker") / (str(index) + "-" + source.name)
        copied = destination / relative
        copied.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, copied)
        public["licenses"].append({"path": relative.as_posix(), "sha256": digest(copied)})
    (destination / "broker-provenance.json").write_text(json.dumps(public, indent=2) + "\n")
