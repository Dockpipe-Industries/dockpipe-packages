#!/usr/bin/env python3
"""Acquire checksum-pinned catalog Go modules before the separate offline build."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

import go_candidate


def prime(root, inputs, environment, target=None):
    environment = dict(environment, GOWORK='off', GOTOOLCHAIN='local')
    if target is not None:
        goos, goarch = target.split('-', 1)
        observed = subprocess.check_output(['go', 'env', 'GOOS', 'GOARCH'], cwd=root,
                                           env=environment, text=True, timeout=15).splitlines()
        if observed != [goos, goarch]:
            raise ValueError('Go dependency acquisition target differs from the native package target')
    lock = json.loads((root / 'release/dependencies.lock.json').read_text())['dockpipe_go_sdk']
    # Network access here installs dependency modules only. The SDK itself is
    # independently authenticated against the committed artifact locks.
    with tempfile.TemporaryDirectory(prefix='catalog-sdk-dependencies-') as temporary:
        work = Path(temporary)
        sdk = work / 'sdk'
        manifest = go_candidate.unpack_sdk(
            inputs / 'sdk.json', inputs / 'sdk.tar.gz', lock['manifest_sha256'], sdk)
        go_candidate.validate_sdk_pin(root, manifest, lock['manifest_sha256'])
        modfile = work / 'catalog.mod'
        shutil.copyfile(root / 'go.mod', modfile)
        modfile.with_suffix('.sum').write_text(
            (root / 'go.sum').read_text() + '\n' + (sdk / 'go.sum').read_text())
        admitted_sums = set(modfile.with_suffix('.sum').read_text().splitlines())
        subprocess.run(['go', 'mod', 'edit', f'-modfile={modfile}', f'-replace=dockpipe={sdk}'],
                       cwd=root, env=environment, check=True, timeout=900)
        subprocess.run(['go', 'mod', 'download', f'-modfile={modfile}', 'all'],
                       cwd=root, env=environment, check=True, timeout=900)
        # With lazy module loading, download-all can omit go.mod files that
        # become necessary when actual imports expand the consumer's graph.
        # Load build and test imports here while downloads are permitted. The
        # later wrapper independently imports the SDK and remains offline.
        subprocess.run(['go', 'list', '-mod=mod', f'-modfile={modfile}',
                        '-buildvcs=false', '-deps', '-test', './...'],
                       cwd=root, env=environment, check=True, timeout=900, stdout=subprocess.DEVNULL)
        resolved_sums = set(modfile.with_suffix('.sum').read_text().splitlines())
        if not resolved_sums <= admitted_sums:
            raise ValueError('Go dependency acquisition added checksums absent from the source and verified SDK')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk-inputs', type=Path, required=True)
    parser.add_argument('--target', choices=['linux-amd64', 'windows-amd64', 'darwin-arm64'], required=True)
    arguments = parser.parse_args()
    prime(Path(__file__).resolve().parents[1], arguments.sdk_inputs, dict(os.environ), arguments.target)


if __name__ == '__main__':
    main()
