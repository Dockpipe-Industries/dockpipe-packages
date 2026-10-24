#!/usr/bin/env python3
"""Install the locked compiled runtime core into a fresh task-private global root."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import tarfile
import tempfile


ROOT = Path(__file__).resolve().parents[1]
MAX_ARCHIVE_BYTES = 16 << 20
MAX_EXPANDED_BYTES = 32 << 20
MAX_MEMBERS = 256


def regular_file(path, maximum):
    if any(parent.is_symlink() for parent in (path, *path.parents)):
        raise ValueError('Runtime core input must not contain links')
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= maximum:
        raise ValueError('Runtime core input must be a bounded regular file')
    return info.st_size


def locked_core():
    path = ROOT / 'release/dependencies.lock.json'
    regular_file(path, 64 << 10)
    document = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(document, dict):
        raise ValueError('Runtime core dependency lock must be an object')
    pin = document.get('dockpipe_runtime_core')
    fields = {'version', 'release', 'url', 'size_bytes', 'sha256'}
    if (type(document.get('schema')) is not int or document['schema'] != 1
            or not isinstance(pin, dict) or set(pin) != fields):
        raise ValueError('Runtime core requires its complete immutable dependency lock')
    version = pin['version']
    if not isinstance(version, str) or not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', version):
        raise ValueError('Runtime core version must be exact')
    release = pin['release']
    if not isinstance(release, str) or not re.fullmatch(
            r'v' + re.escape(version) + r'-staging\.[0-9]+\.[0-9]+\.[0-9a-f]{12}', release):
        raise ValueError('Runtime core requires its exact immutable staging release')
    name = 'dockpipe-core-' + version + '.tar.gz'
    expected_url = ('https://github.com/Dockpipe-Industries/dockpipe/releases/download/'
                    + release + '/' + name)
    if pin['url'] != expected_url:
        raise ValueError('Runtime core URL differs from its official versioned asset')
    if (type(pin['size_bytes']) is not int or not 0 < pin['size_bytes'] <= MAX_ARCHIVE_BYTES
            or not isinstance(pin['sha256'], str)
            or not re.fullmatch(r'[0-9a-f]{64}', pin['sha256'])):
        raise ValueError('Runtime core requires bounded size and exact SHA-256')
    return pin, name


def verify(payload, pin):
    if regular_file(payload, MAX_ARCHIVE_BYTES) != pin['size_bytes']:
        raise ValueError('Runtime core archive size differs from its lock')
    with payload.open('rb') as stream:
        content = stream.read(MAX_ARCHIVE_BYTES + 1)
    if len(content) != pin['size_bytes'] or hashlib.sha256(content).hexdigest() != pin['sha256']:
        raise ValueError('Runtime core archive checksum differs from its lock')
    names = set()
    expanded = 0
    with tarfile.open(payload, mode='r|gz') as archive:
        for member in archive:
            name = member.name.rstrip('/') if member.isdir() else member.name
            path = PurePosixPath(name)
            if (len(names) >= MAX_MEMBERS or name in names or not name
                    or path.is_absolute() or '..' in path.parts or '\\' in name or ':' in name
                    or path.as_posix() != name or path.parts[0] != 'core'
                    or not (member.isfile() or member.isdir())):
                raise ValueError('Runtime core archive contains unsafe or excess entries')
            names.add(name)
            expanded += member.size
            if member.size < 0 or expanded > MAX_EXPANDED_BYTES:
                raise ValueError('Runtime core archive exceeds expanded capacity')
    if 'core/package.yml' not in names or not any(name.startswith('core/runtimes/') for name in names):
        raise ValueError('Runtime core archive lacks its manifest or runtime assets')


def install(output, archive=None):
    pin, name = locked_core()
    output = output.absolute()
    if output.exists() or any(parent.is_symlink() for parent in (output, *output.parents)):
        raise ValueError('Runtime core requires a fresh task-private output root without links')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='runtime-core-', dir=output.parent) as temporary:
        staged = Path(temporary)
        payload = staged / 'payload.tar.gz'
        if archive is None:
            curl = shutil.which('curl')
            if not curl:
                raise ValueError('Runtime core acquisition requires curl')
            subprocess.run([curl, '--fail', '--silent', '--show-error', '--location',
                            '--proto', '=https', '--proto-redir', '=https',
                            '--connect-timeout', '20', '--max-time', '120',
                            '--max-filesize', str(pin['size_bytes']),
                            '--output', str(payload), pin['url']], check=True, timeout=130)
        else:
            if regular_file(archive, MAX_ARCHIVE_BYTES) != pin['size_bytes']:
                raise ValueError('Runtime core archive size differs from its lock')
            shutil.copyfile(archive, payload)
        verify(payload, pin)
        tree = staged / 'global'
        destination = tree / 'packages/core' / name
        destination.parent.mkdir(parents=True)
        shutil.move(payload, destination)
        tree.rename(output)
    environment_file = os.environ.get('GITHUB_ENV')
    if environment_file:
        with open(environment_file, 'a', encoding='utf-8') as environment:
            environment.write('DOCKPIPE_GLOBAL_ROOT=' + str(output) + '\n')
    print(str(output))
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--archive', type=Path)
    options = parser.parse_args()
    try:
        install(options.output, options.archive)
    except (OSError, ValueError, tarfile.TarError, subprocess.SubprocessError) as error:
        parser.exit(1, 'Runtime core installation failed: ' + str(error) + '\n')
