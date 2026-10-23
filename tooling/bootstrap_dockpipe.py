#!/usr/bin/env python3
"""Install the repository's exact Dockpipe release using its checksum lock."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import tarfile
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def install(output, archive=None):
    config = json.loads((ROOT / 'dockpipe.config.json').read_text())
    lock = json.loads((ROOT / 'dockpipe.lock.json').read_text())
    version = config['dockpipe_version']
    if lock['schema'] != 1 or lock['dockpipe']['version'] != version:
        raise ValueError('Dockpipe config and lock version must agree')
    system = platform.system().lower()
    architecture = {'x86_64': 'amd64', 'amd64': 'amd64', 'aarch64': 'arm64', 'arm64': 'arm64'}.get(platform.machine().lower())
    target = system + '/' + str(architecture)
    if target not in lock['dockpipe']['artifacts']:
        raise ValueError('No locked Dockpipe release for ' + target)
    entry = lock['dockpipe']['artifacts'][target]
    if not entry['url'].startswith('https://github.com/Dockpipe-Industries/dockpipe/releases/download/'):
        raise ValueError('Expected an official Dockpipe release URL')
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    executable = output / entry['executable']
    with tempfile.TemporaryDirectory(prefix='dockpipe-install-') as temporary:
        payload = Path(temporary) / 'release'
        if archive is not None:
            if archive.stat().st_size != entry['size_bytes']:
                raise ValueError('Locked archive size mismatch')
            payload.write_bytes(archive.read_bytes())
        else:
            with urllib.request.urlopen(entry['url'], timeout=60) as response, payload.open('wb') as destination:
                remaining = entry['size_bytes']
                while remaining:
                    chunk = response.read(min(1024 * 1024, remaining))
                    if not chunk:
                        raise ValueError('Truncated Dockpipe release')
                    destination.write(chunk)
                    remaining -= len(chunk)
                if response.read(1):
                    raise ValueError('Dockpipe release exceeds locked size')
        if hashlib.sha256(payload.read_bytes()).hexdigest() != entry['sha256']:
            raise ValueError('Dockpipe release checksum mismatch')
        if system == 'windows':
            with zipfile.ZipFile(payload) as bundle:
                member = bundle.getinfo(entry['executable'])
                if member.file_size != entry['executable_size_bytes']:
                    raise ValueError('Locked executable size mismatch')
                data = bundle.read(member)
        else:
            with tarfile.open(payload) as bundle:
                member = bundle.getmember(entry['executable'])
                if not member.isfile() or member.size != entry['executable_size_bytes']:
                    raise ValueError('Locked executable is not a regular file of the expected size')
                with bundle.extractfile(member) as source:
                    data = source.read()
        candidate = Path(temporary) / entry['executable']
        candidate.write_bytes(data)
        candidate.chmod(0o755)
        actual = subprocess.check_output([str(candidate), '--version'], text=True, timeout=15).strip()
        if actual != version:
            raise ValueError('Downloaded Dockpipe version differs from repository pin: ' + actual)
        executable.write_bytes(data)
        executable.chmod(0o755)
    print(str(executable))
    environment_file = os.environ.get('GITHUB_ENV')
    if environment_file:
        with open(environment_file, 'a', encoding='utf-8') as environment:
            environment.write('DOCKPIPE_BIN=' + str(executable) + '\n')
    return executable


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--archive', type=Path, help='Verify a previously downloaded archive instead of fetching it')
    options = parser.parse_args()
    install(options.output, options.archive)
