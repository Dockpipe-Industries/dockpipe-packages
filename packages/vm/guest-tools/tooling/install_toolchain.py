#!/usr/bin/env python3
"""Extract only exact official tool archives after independently checking their locks."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import subprocess
import tempfile

import py7zr

ROOT = Path(__file__).resolve().parents[1]
MAXIMUM_FILES = 60000
MAXIMUM_EXPANDED_BYTES = 3 * 1024 * 1024 * 1024


def verify_archive(path, entry):
    if not path.is_file() or path.is_symlink() or path.stat().st_size != entry['size_bytes']:
        raise ValueError('Tool archive does not match its locked size: ' + entry['name'])
    checksum = hashlib.sha256()
    with path.open('rb') as source:
        while chunk := source.read(1024 * 1024):
            checksum.update(chunk)
    if checksum.hexdigest() != entry['sha256']:
        raise ValueError('Tool archive does not match its locked SHA-256: ' + entry['name'])


def inspect_archive(archive):
    seen = set()
    expanded = 0
    for entry in archive.files:
        name = entry.filename
        path = PurePosixPath(name)
        if not name or path.is_absolute() or '..' in path.parts or ':' in name or '\\' in name:
            raise ValueError('Tool archive contains an unsafe path')
        if name.casefold() in seen or entry.is_symlink or entry.is_junction or entry.is_socket:
            raise ValueError('Tool archive contains duplicate or redirected entries')
        seen.add(name.casefold())
        expanded += entry.uncompressed
        if len(seen) > MAXIMUM_FILES or expanded > MAXIMUM_EXPANDED_BYTES:
            raise ValueError('Tool archive exceeds its extraction capacity')


def install(archives, output):
    if platform.system() != 'Windows':
        raise ValueError('This toolchain must be installed and qualified on native Windows')
    if py7zr.__version__ != '1.0.0':
        raise ValueError('Use the SHA-256-locked py7zr 1.0.0 extractor environment')
    lock = json.loads((ROOT / 'tooling/toolchain.lock.json').read_text())
    expected_receipt = {'schema': 1, 'lock_sha256': hashlib.sha256(
        (ROOT / 'tooling/toolchain.lock.json').read_bytes()).hexdigest()}
    if output.exists():
        receipt = output / 'toolchain-receipt.json'
        if not receipt.is_file() or json.loads(receipt.read_text()) != expected_receipt:
            raise ValueError('Existing toolchain does not match the lock; select a fresh output directory')
        print('Retained the previously verified toolchain: ' + str(output))
        return
    for entry in lock['archives']:
        verify_archive(archives / (entry['name'] + '.7z'), entry)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='guest-tools-qt-', dir=output.parent) as temporary:
        staging = Path(temporary) / 'Qt'
        staging.mkdir()
        for entry in lock['archives']:
            print('Extracting locked ' + entry['name'], flush=True)
            destination = staging / entry['extract_to_relative']
            destination.mkdir(parents=True, exist_ok=True)
            with py7zr.SevenZipFile(archives / (entry['name'] + '.7z')) as archive:
                inspect_archive(archive)
                archive.extractall(destination)
        for relative in lock['expected_executables'].values():
            if not (staging / relative).is_file():
                raise ValueError('Toolchain is missing a locked executable: ' + relative)
        # Qt's official binary archives use a relative runtime prefix when qt.conf
        # is adjacent to qmake and the deployment tools. CMake targets resolve their
        # imports relative to the installed config. No mutable package selection or
        # compiler source build is involved in this relocation.
        (staging / '6.10.3/mingw_64/bin/qt.conf').write_text('[Paths]\nPrefix=..\n', encoding='utf-8')
        (staging / 'toolchain-receipt.json').write_text(json.dumps(expected_receipt, indent=2) + '\n')
        staging.rename(output)
    environment = dict(os.environ)
    compiler_bin = output / 'Tools/mingw1310_64/bin'
    qt_bin = output / '6.10.3/mingw_64/bin'
    environment['PATH'] = str(compiler_bin) + os.pathsep + str(qt_bin) + os.pathsep + environment['PATH']
    qmake = qt_bin / 'qmake.exe'
    version = subprocess.check_output([str(qmake), '-query', 'QT_VERSION'], env=environment, text=True).strip()
    prefix = subprocess.check_output([str(qmake), '-query', 'QT_INSTALL_PREFIX'], env=environment, text=True).strip()
    if version != lock['qt_version'] or Path(prefix).resolve() != (output / '6.10.3/mingw_64').resolve():
        raise ValueError('Qt relocation or executable version differs from the selected toolchain')
    print('Verified Qt ' + version + ' at ' + prefix, flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archives', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    arguments = parser.parse_args()
    install(arguments.archives.resolve(), arguments.output.resolve())
