#!/usr/bin/env python3
"""Prime external Go dependencies, then build through the catalog's locked SDK wrapper."""
import os
from pathlib import Path
import platform
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'tooling'))
import catalog_go_dependencies
import vm_go


def build_catalog(inputs, environment):
    catalog_go_dependencies.prime(ROOT, inputs, environment)
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
