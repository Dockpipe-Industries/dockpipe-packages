#!/usr/bin/env python3
"""Acquire locked official inputs and run the native candidate producer."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request

import build_candidate


# This reviewed source includes the native Windows environment repair f01897a4c
# and allocation fix 0684173fb. Other staging releases need their own review.
REVIEWED_STAGING_CLI_SOURCES = {
    ('0.6.33', 'v0.6.33-staging.40247668385.1.b107ea64782c'):
        'b107ea64782c5a153440fb786791f1500a55416e',
}


def download(entry, destination):
    if (not entry['url'].startswith('https://download.qt.io/online/qtsdkrepository/')
            or type(entry['size_bytes']) is not int or not 0 < entry['size_bytes'] <= 256 << 20
            or not build_candidate.candidate.hash_value(entry['sha256']) or destination.exists()):
        raise ValueError('Official tool archive admission failed')
    checksum = hashlib.sha256()
    remaining = entry['size_bytes']
    with urllib.request.urlopen(entry['url'], timeout=60) as source, destination.open('xb') as output:
        while remaining:
            chunk = source.read(min(1 << 20, remaining))
            if not chunk:
                raise ValueError('Official tool archive is truncated')
            output.write(chunk)
            checksum.update(chunk)
            remaining -= len(chunk)
        if source.read(1) or checksum.hexdigest() != entry['sha256']:
            raise ValueError('Official tool archive differs from the committed lock')


def require_public_cli_candidate(source):
    config = json.loads((source / 'dockpipe.config.json').read_bytes())
    lock = json.loads((source / 'dockpipe.lock.json').read_bytes())
    pin = lock.get('dockpipe', {})
    version = config.get('dockpipe_version')
    release = pin.get('release', '')
    match = re.fullmatch(r'cli-candidate-((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))-staging\.([0-9a-f]{40})', release)
    candidate_matches = (
        match is not None
        and tuple(map(int, match[1].split('.'))) >= (0, 6, 26)
        and version == match[1] + '-staging.cli.sha' + match[2][:12]
    )
    reviewed_source = REVIEWED_STAGING_CLI_SOURCES.get((version, release))
    staging_matches = (
        reviewed_source is not None
        and release.endswith('.' + reviewed_source[:12])
    )
    if (lock.get('schema') != 1 or pin.get('version') != version
            or not (candidate_matches or staging_matches)):
        raise ValueError('A verified public native-environment-repaired CLI candidate pin is required before tool acquisition')
    entry = pin.get('artifacts', {}).get('windows/amd64', {})
    expected_url = ('https://github.com/Dockpipe-Industries/dockpipe/releases/download/' + release
                    + '/dockpipe_' + version + '_windows_amd64.zip')
    if (entry.get('url') != expected_url or entry.get('executable') != 'dockpipe.exe'
            or not build_candidate.candidate.hash_value(entry.get('sha256'))
            or type(entry.get('size_bytes')) is not int or not 0 < entry['size_bytes'] <= 64 << 20
            or type(entry.get('executable_size_bytes')) is not int
            or not 0 < entry['executable_size_bytes'] <= 64 << 20):
        raise ValueError('Windows CLI acquisition requires its exact owning public archive hash and sizes')
    # The shared bootstrap remains responsible for actual public transfer,
    # archive/executable integrity and native version execution. This early guard
    # prevents expensive Qt acquisition with the knowingly incompatible old pin.
    return entry


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    arguments = parser.parse_args()
    source = arguments.source
    work = arguments.work
    if not source.is_absolute() or not work.is_absolute() or work.exists():
        raise ValueError('Select an explicit source and fresh native workspace')
    build_candidate.hosted_identity(os.environ)
    require_public_cli_candidate(source)
    work.mkdir(parents=True)
    component = source / build_candidate.COMPONENT
    lock = json.loads((component / 'tooling/toolchain.lock.json').read_bytes())
    if len(lock['archives']) != 6:
        raise ValueError('Expected the complete six-archive official toolchain lock')
    archives = work / 'archives'
    archives.mkdir()
    for entry in lock['archives']:
        if entry['name'] not in {'qtbase', 'qttools', 'mingw-runtime', 'mingw', 'cmake', 'ninja'}:
            raise ValueError('Unrecognized toolchain archive')
        download(entry, archives / (entry['name'] + '.7z'))
    python = Path(sys.executable)
    subprocess.run([str(python), '-m', 'pip', 'install', '--require-hashes',
                    '-r', str(component / 'tooling/extractor-requirements.lock.txt')],
                   check=True, timeout=300)
    qt = work / 'Qt'
    subprocess.run([str(python), '-B', str(component / 'tooling/install_toolchain.py'),
                    '--archives', str(archives), '--output', str(qt)], check=True, timeout=600)
    subprocess.run([str(python), '-B', str(source / 'tooling/bootstrap_dockpipe.py'),
                    '--output', str(work / 'cli')], check=True, timeout=180)
    binary = work / 'cli/dockpipe.exe'
    # bootstrap_dockpipe independently verifies the locked public archive,
    # executable size and native version before this executable digest is used.
    cli_sha = build_candidate.candidate.digest(binary.read_bytes())
    git = shutil.which('git.exe')
    if not git:
        raise ValueError('Native Git executable is required to admit actual source bytes')
    build_candidate.build(source, os.environ.get('GITHUB_SHA', ''), Path(git), binary,
                          cli_sha, qt, work / 'candidate')


if __name__ == '__main__':
    main()
