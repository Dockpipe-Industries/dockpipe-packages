#!/usr/bin/env python3
"""Run the supported native Windows catalog and VM compatibility acceptance slice."""
import os
from pathlib import Path
import platform
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'tooling'))
import vm_go


def main():
    host_system = platform.system()
    if host_system != 'Windows':
        raise ValueError('The Windows acceptance workflow requires a native Windows host')
    environment = dict(os.environ, GOWORK='off', GOTOOLCHAIN='local')
    subprocess.run([sys.executable, str(ROOT / 'workflows/codeql.build/assets/scripts/build.py')],
                   cwd=ROOT, env=environment, check=True)
    subprocess.run([sys.executable, str(ROOT / 'tooling/catalog_go.py'), '--', 'test',
                    './packages/secrets/tools/...', './packages/remote/tools/...'],
                   cwd=ROOT, env=environment, check=True)
    for arguments in vm_go.test_arguments(host_system):
        subprocess.run(arguments, cwd=ROOT / 'packages/vm/tools',
                       env=environment, check=True)


if __name__ == '__main__':
    main()
