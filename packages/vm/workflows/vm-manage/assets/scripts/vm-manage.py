#!/usr/bin/env python3
"""CLI access to the same installed provider operations used by the VM page."""
import os
from pathlib import Path
import re
import subprocess
import sys


def main():
    action = os.environ.get('DOCKPIPE_VM_ACTION', 'snapshot')
    if action not in ('snapshot', 'repair', 'open', 'shutdown', 'clipboard-start', 'clipboard-stop', 'resolution'):
        raise ValueError('Choose snapshot, repair, open, shutdown, clipboard-start, clipboard-stop or resolution')
    if action != 'snapshot' and not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,127}', os.environ.get('DOCKPIPE_VM_TARGET_RUN_ID') or os.environ.get('DOCKPIPE_SURFACE_RESOURCE_ID', '')):
        raise ValueError('Set DOCKPIPE_SURFACE_RESOURCE_ID to a VM run ID from the status snapshot')
    program = Path(os.environ.get('DOCKPIPE_BIN', ''))
    workspace = Path(os.environ.get('DOCKPIPE_WORKDIR', ''))
    if not program.is_absolute() or not program.is_file() or not workspace.is_absolute():
        raise ValueError('Dockpipe executable and workspace context are required')
    result = subprocess.run([str(program), 'scope', 'resolver', 'qemu', 'assets-dir', '--workdir', str(workspace)],
                            capture_output=True, timeout=10, check=False)
    if result.returncode or len(result.stdout) > 16384:
        raise ValueError('Install the QEMU resolver to use VM management')
    assets = Path(result.stdout.decode('utf-8').strip())
    suffix = '.exe' if os.name == 'nt' else ''
    manager = assets / 'tooling/bin' / ('dockpipe-vm-manager' + suffix)
    if not assets.is_absolute() or not manager.is_file() or manager.is_symlink():
        raise ValueError('The installed QEMU resolver is missing its native management tool')
    return subprocess.run([str(manager), action], cwd=workspace, timeout=30, check=False).returncode


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        print('VM management: ' + str(error), file=sys.stderr)
        sys.exit(1)
