#!/usr/bin/env python3
"""Prime external Go dependencies, then build through the catalog's locked SDK wrapper."""
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'tooling'))
import go_candidate
import vm_go


def build_catalog(inputs, environment):
    lock = json.loads((ROOT / 'release/dependencies.lock.json').read_text())['dockpipe_go_sdk']
    # Network access here installs dependency modules only. The SDK itself is
    # independently authenticated against the committed artifact locks.
    with tempfile.TemporaryDirectory(prefix='codeql-sdk-dependencies-') as temporary:
        work = Path(temporary)
        sdk = work / 'sdk'
        manifest = go_candidate.unpack_sdk(
            inputs / 'sdk.json', inputs / 'sdk.tar.gz', lock['manifest_sha256'], sdk)
        go_candidate.validate_sdk_pin(ROOT, manifest, lock['manifest_sha256'])
        modfile = work / 'catalog.mod'
        shutil.copyfile(ROOT / 'go.mod', modfile)
        modfile.with_suffix('.sum').write_text(
            (ROOT / 'go.sum').read_text() + '\n' + (sdk / 'go.sum').read_text())
        subprocess.run(['go', 'mod', 'edit', f'-modfile={modfile}', f'-replace=dockpipe={sdk}'],
                       cwd=ROOT, env=environment, check=True)
        subprocess.run(['go', 'mod', 'download', f'-modfile={modfile}', 'all'],
                       cwd=ROOT, env=environment, check=True)
        # With lazy module loading, download-all can omit go.mod files that
        # become necessary when actual imports expand the consumer's graph.
        # Load build and test imports here while downloads are permitted. The
        # later wrapper independently imports the SDK and remains offline.
        subprocess.run(['go', 'list', '-mod=mod', f'-modfile={modfile}',
                        '-deps', '-test', './...'],
                       cwd=ROOT, env=environment, check=True, timeout=900, stdout=subprocess.DEVNULL)
    subprocess.run([sys.executable, str(ROOT / 'tooling/catalog_go.py'),
                    '--sdk-inputs', str(inputs), '--', 'build', './...'],
                   cwd=ROOT, env=environment, check=True)


def build_vm_tools(environment, host_system):
    vm_tools = ROOT / 'packages/vm/tools'
    subprocess.run(['go', 'mod', 'download'], cwd=vm_tools, env=environment, check=True)
    subprocess.run(vm_go.build_arguments(host_system),
                   cwd=vm_tools, env=environment, check=True)


def main():
    selected_inputs = os.environ.get('DOCKPIPE_GO_SDK_INPUTS')
    if not selected_inputs:
        raise ValueError('DOCKPIPE_GO_SDK_INPUTS must select the fetched, pinned public SDK inputs')
    inputs = Path(selected_inputs)
    if not inputs.is_absolute():
        raise ValueError('DOCKPIPE_GO_SDK_INPUTS must be absolute')
    environment = dict(os.environ, GOWORK='off', GOTOOLCHAIN='local')
    build_catalog(inputs, environment)
    build_vm_tools(environment, platform.system())


if __name__ == '__main__':
    main()
