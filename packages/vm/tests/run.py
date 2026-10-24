#!/usr/bin/env python3
"""Keep legacy qualification host-specific; test the native macOS manager/display path."""
import os
from pathlib import Path
import platform
import subprocess
import sys


def test_commands(package, host, environment):
    repository = package.parents[1]
    if host == 'Linux':
        return [(package, ['bash', str(package / 'tests/run.sh')])]
    if host == 'Windows':
        sys.path.insert(0, str(repository / 'tooling'))
        import vm_go
        return ([(package / 'tools', arguments) for arguments in vm_go.test_arguments(host)]
                + [(package, [sys.executable, str(package / 'tests/test_runtime_compatibility.py')])])
    if host != 'Darwin':
        raise ValueError('VM package tests have no qualified contract for this host')
    selected = environment.get('DOCKPIPE_BIN', '')
    cli = Path(selected)
    if not selected or not cli.is_absolute() or not cli.is_file() or not os.access(cli, os.X_OK):
        raise ValueError('Native VM tests require an absolute pinned DOCKPIPE_BIN')
    required = ('DOCKPIPE_VM_DISPLAY_SDK_PREFIX', 'DOCKPIPE_VM_HOST_QT_PREFIX',
                'DOCKPIPE_GIO_PREFIX', 'DOCKPIPE_VM_DBUS_DAEMON_BIN',
                'DOCKPIPE_VM_DBUS_PROVENANCE', 'DOCKPIPE_VM_GUEST_TOOLS_ARCHIVE',
                'DOCKPIPE_VM_GUEST_TOOLS_DESCRIPTOR', 'DOCKPIPE_GO_SDK_INPUTS')
    if any(not environment.get(name) for name in required):
        raise ValueError('Native macOS tests require the complete explicitly selected display and delivery profile')
    return [
        (package, [sys.executable, str(package / 'tests/test_runtime_compatibility.py')]),
        (repository, [sys.executable, str(repository / 'tooling/catalog_go.py'), '--',
                      'test', './packages/vm/management/...']),
        # The owning source hook performs native Qt CTest, ABI/runtime closure,
        # broker provenance and Guest Tools delivery-integrity checks. It does
        # not invoke the separate Linux-only provisioning qualification module.
        (repository, [str(cli), 'package', 'build', 'source', '--workdir', str(repository), '--only', 'vm']),
    ]


def main():
    package = Path(os.environ.get('DOCKPIPE_PACKAGE_ROOT', Path(__file__).resolve().parents[1]))
    for directory, arguments in test_commands(package, platform.system(), os.environ):
        subprocess.run(arguments, cwd=directory, check=True, timeout=1800)


if __name__ == '__main__':
    main()
