#!/usr/bin/env python3
"""Run catalog Go tools against the immutable, checksum-verified SDK artifact."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

import go_candidate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk-inputs', type=Path, default=None)
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    inputs = args.sdk_inputs or Path(os.environ.get('DOCKPIPE_GO_SDK_INPUTS', root / 'build/sdk-inputs'))
    if not (inputs / 'sdk.json').is_file() or not (inputs / 'sdk.tar.gz').is_file():
        parser.error('Pinned SDK inputs missing; run tooling/fetch_sdk_release.py --output build/sdk-inputs or set DOCKPIPE_GO_SDK_INPUTS to verified release inputs')
    lock = json.loads((root / 'release/dependencies.lock.json').read_text())['dockpipe_go_sdk']
    with tempfile.TemporaryDirectory(prefix='catalog-sdk-') as temporary:
        directory = Path(temporary)
        sdk = directory / 'sdk'
        manifest = go_candidate.unpack_sdk(inputs / 'sdk.json', inputs / 'sdk.tar.gz', lock['manifest_sha256'], sdk)
        go_candidate.validate_sdk_pin(root, manifest, lock['manifest_sha256'])
        modfile = directory / 'catalog.mod'
        shutil.copyfile(root / 'go.mod', modfile)
        modfile.with_suffix('.sum').write_text((root / 'go.sum').read_text() + '\n' + (sdk / 'go.sum').read_text())
        environment = dict(os.environ, GOWORK='off', GOPROXY='off', GOSUMDB='off')
        subprocess.run(['go', 'mod', 'edit', f'-modfile={modfile}', f'-replace=dockpipe={sdk}'], cwd=root, env=environment, check=True)
        arguments = args.arguments
        if arguments and arguments[0] == '--':
            arguments = arguments[1:]
        if not arguments or arguments[0] not in ('build', 'test'):
            parser.error('expected build or test command')
        command = ['go', arguments[0], '-mod=mod', f'-modfile={modfile}', *arguments[1:]]
        subprocess.run(command, cwd=root, env=environment, check=True)


if __name__ == '__main__':
    main()
