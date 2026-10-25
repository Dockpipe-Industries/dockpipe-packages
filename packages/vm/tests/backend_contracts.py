#!/usr/bin/env python3
"""Run package-owned backend unit contracts without launching an actual VM."""
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile

LINUX_TESTS = (
    'test_vm_darwin.sh',
    'test_vm_guest_tools_media.sh',
    'test_vm_session_reporter.sh',
    'test_vm_boot_display.sh',
    'test_vm_boot_display_args.sh',
    'test_vm_posix_cleanup.sh',
    'test_vm_windows_media_paths.sh',
    'test_vm_guest_preparation.sh',
    'test_windows_guest_services.sh',
)
WINDOWS_TESTS = (
    'test_windows_guest_services.ps1',
    'test_windows_guest_agent_repair.ps1',
    'test_vm_windows_exit_status.ps1',
)


def test_commands(tests, host):
    if host == 'Linux':
        return [['bash', str(tests / name)] for name in LINUX_TESTS] + [
            [sys.executable, str(tests / 'test_guest_agent_reproducibility.py')]]
    if host == 'Windows':
        return [['pwsh', '-NoProfile', '-NonInteractive', '-File', str(tests / name)]
                for name in WINDOWS_TESTS] + [
                    [sys.executable, str(tests / 'test_guest_agent_reproducibility.py')]]
    raise ValueError('Backend contracts require the Linux or native Windows test lane')


def main():
    tests = Path(__file__).resolve().parent
    environment = os.environ.copy()
    selected = environment.get('DOCKPIPE_TEST_DOCKPIPE_BIN') or environment.get('DOCKPIPE_BIN', '')
    binary = Path(selected)
    if not selected or not binary.is_absolute() or not binary.is_file():
        raise ValueError('Backend tests require an explicitly selected pinned DOCKPIPE_BIN')
    for name in list(environment):
        if name.startswith('DOCKPIPE_') and not name.startswith(('DOCKPIPE_TEST_', 'DOCKPIPE_GO_SDK_')):
            environment.pop(name)
    environment['DOCKPIPE_BIN'] = str(binary)
    environment['DOCKPIPE_TEST_DOCKPIPE_BIN'] = str(binary)
    with tempfile.TemporaryDirectory(prefix='vm-backend-contracts-') as temporary:
        home = Path(temporary) / 'home'
        home.mkdir()
        environment['HOME'] = str(home)
        for arguments in test_commands(tests, platform.system()):
            print('Backend contract: ' + Path(arguments[-1]).name, flush=True)
            subprocess.run(arguments, cwd=tests.parent, env=environment, check=True, timeout=300)


if __name__ == '__main__':
    main()
