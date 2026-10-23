#!/usr/bin/env python3
"""Verify an explicit immutable Windows Guest Tools artifact before packaging it."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import stat
import tempfile
import zipfile

MAX_BYTES = 256 * 1024 * 1024
MAX_FILES = 512


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_document(path, maximum):
    if path.is_symlink() or not path.is_file() or path.stat().st_size > maximum:
        raise ValueError('Artifact metadata must be a bounded regular file')
    return json.loads(path.read_bytes())


def relative_name(name):
    parts = PurePosixPath(name).parts
    if not parts or name != '/'.join(parts) or name.startswith('/') or any(
            part in ('.', '..') for part in parts) or any(character in name for character in '\\:\x00\r\n'):
        raise ValueError('Artifact contains an unsafe relative path')
    return name


def materialize(artifact, descriptor_path, destination):
    descriptor = read_document(descriptor_path, 65536)
    expected = {'schema': 1, 'product': 'DockpipeGuestTools', 'version': '1.0.0',
                'platform': 'windows-amd64', 'qt_version': '6.10.3',
                'entrypoint': 'dockpipe-guest-tools.exe', 'installer': 'install-guest-tools.ps1',
                'media_subdirectory': 'GuestTools'}
    if any(descriptor.get(key) != value for key, value in expected.items()):
        raise ValueError('Unsupported Guest Tools artifact contract')
    if artifact.is_symlink() or not artifact.is_file() or artifact.stat().st_size > MAX_BYTES:
        raise ValueError('Guest Tools archive must be a bounded regular file')
    archive_bytes = artifact.read_bytes()
    archive = descriptor.get('archive', {})
    if archive.get('name') != artifact.name or archive.get('size_bytes') != len(archive_bytes) or archive.get('sha256') != digest(archive_bytes):
        raise ValueError('Guest Tools archive identity mismatch')
    if destination.exists() or destination.is_symlink():
        raise ValueError('Guest Tools destination already exists')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.guest-tools-', dir=destination.parent) as temporary:
        staging = Path(temporary) / 'bundle'
        staging.mkdir()
        with zipfile.ZipFile(artifact) as source:
            entries = source.infolist()
            if not entries or len(entries) > MAX_FILES or sum(entry.file_size for entry in entries) > MAX_BYTES:
                raise ValueError('Guest Tools archive exceeds its file or size contract')
            seen = set()
            for entry in entries:
                name = relative_name(entry.filename)
                mode = entry.external_attr >> 16
                if name.casefold() in seen or entry.is_dir() or (stat.S_IFMT(mode) not in (0, stat.S_IFREG)):
                    raise ValueError('Guest Tools archive contains duplicate or nonregular entries')
                seen.add(name.casefold())
                target = staging / name
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.open(entry) as incoming, target.open('xb') as outgoing:
                    shutil.copyfileobj(incoming, outgoing, 1024 * 1024)
                if target.stat().st_size != entry.file_size:
                    raise ValueError('Guest Tools archive entry size mismatch')
        inventory_path = staging / 'inventory.json'
        inventory = read_document(inventory_path, 1024 * 1024)
        if digest(inventory_path.read_bytes()) != descriptor.get('inventory_sha256') or any(
                inventory.get(key) != value for key, value in expected.items() if key in ('schema', 'product', 'version', 'platform', 'qt_version')):
            raise ValueError('Guest Tools inventory identity mismatch')
        files = inventory.get('files')
        if not isinstance(files, list) or not files or len(files) >= MAX_FILES:
            raise ValueError('Guest Tools inventory has an invalid file count')
        listed = {'inventory.json'}
        for entry in files:
            name = relative_name(entry['path'])
            path = staging / name
            if name.casefold() in listed or not path.is_file() or path.stat().st_size != entry['bytes'] or digest(path.read_bytes()) != entry['sha256']:
                raise ValueError('Guest Tools file integrity mismatch')
            listed.add(name.casefold())
        if listed != seen:
            raise ValueError('Guest Tools archive and inventory differ')
        required = {'dockpipe-guest-tools.exe', 'install-guest-tools.ps1', 'licenses/third-party.txt'}
        if not required.issubset(listed):
            raise ValueError('Guest Tools artifact is incomplete')
        staging.rename(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--descriptor', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True)
    arguments = parser.parse_args()
    materialize(arguments.archive, arguments.descriptor, arguments.destination)


if __name__ == '__main__':
    main()
