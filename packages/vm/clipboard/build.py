#!/usr/bin/env python3
"""Build a closed Linux Qt clipboard bridge from an explicitly selected Qt 6.10.3 kit."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def run(arguments, **kwargs):
    return subprocess.check_output(arguments, text=True, **kwargs).strip()


def dependency_environment(prefix, inherited):
    environment = dict(inherited)
    directories = [str(prefix / 'lib')]
    if inherited.get('LD_LIBRARY_PATH'):
        directories.append(inherited['LD_LIBRARY_PATH'])
    environment['LD_LIBRARY_PATH'] = os.pathsep.join(directories)
    return environment


def require_resolved_dependencies(output):
    if 'not found' not in output:
        return
    missing = re.findall(r'^\s*([A-Za-z0-9_.+-]{1,128})\s+=>\s+not found\s*$', output, re.MULTILINE)
    if not missing or len(missing) > 32:
        raise RuntimeError('Clipboard runtime dependency scan reported unresolved entries')
    raise RuntimeError('Clipboard runtime dependencies unresolved: ' + ', '.join(missing))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument('--output', type=Path)
    selection.add_argument('--output-bin', type=Path)
    args = parser.parse_args()
    supplied = os.environ.get('DOCKPIPE_VM_HOST_QT_PREFIX', '')
    prefix = Path(supplied)
    if not supplied or not prefix.is_absolute():
        parser.error('DOCKPIPE_VM_HOST_QT_PREFIX must explicitly select an absolute Qt 6.10.3 kit')
    prefix = prefix.resolve(strict=True)
    if run([str(prefix / 'bin/qtpaths6'), '--qt-version']) != '6.10.3':
        parser.error('Host clipboard requires exactly Qt 6.10.3')
    source = Path(__file__).resolve().parent
    output = (args.output or args.output_bin.parent).resolve()
    binary_root = output / 'bin'
    binary_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='dockpipe-vm-clipboard-') as temporary:
        build = Path(temporary)
        subprocess.run(['cmake', '-S', str(source), '-B', str(build), '-DCMAKE_BUILD_TYPE=Release',
                        f'-DCMAKE_PREFIX_PATH={prefix}', f'-DQt6_DIR={prefix}/lib/cmake/Qt6',
                        '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF'], check=True)
        subprocess.run(['cmake', '--build', str(build), '--parallel', '2'], check=True)
        shutil.copy2(build / 'dockpipe-vm-clipboard', binary_root / 'dockpipe-vm-clipboard')
    artifacts = [binary_root / 'dockpipe-vm-clipboard']
    categories = ['platforms', 'wayland-shell-integration', 'wayland-decoration-client',
                  'wayland-graphics-integration-client', 'xcbglintegrations']
    for category in categories:
        directory = prefix / 'plugins' / category
        if not directory.is_dir():
            continue
        for plugin in directory.glob('*.so'):
            if category == 'platforms' and plugin.name not in ('libqxcb.so', 'libqwayland.so', 'libqoffscreen.so'):
                continue
            destination = output / 'plugins' / category / plugin.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(plugin, destination)
            artifacts.append(destination)
    pending = list(artifacts)
    copied = set()
    while pending:
        binary = pending.pop()
        environment = dependency_environment(prefix, os.environ)
        dependencies = run(['ldd', str(binary)], env=environment)
        require_resolved_dependencies(dependencies)
        for library in re.findall(r'=>\s+(/\S+)', dependencies):
            path = Path(library).resolve()
            if not path.is_relative_to(prefix):
                continue
            name = Path(library).name
            if name in copied:
                continue
            copied.add(name)
            destination = output / 'lib' / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
            pending.append(destination)
            artifacts.append(destination)
    (binary_root / 'qt.conf').write_text('[Paths]\nPrefix=..\nLibraries=lib\nPlugins=plugins\n', encoding='utf-8')
    artifacts.append(binary_root / 'qt.conf')
    for binary in artifacts:
        if binary.suffix == '.conf':
            continue
        dynamic = run(['readelf', '-d', str(binary)])
        if str(prefix) in dynamic:
            raise RuntimeError('Clipboard artifact retains a Qt kit runtime path')
    inventory = [{'path': str(path.relative_to(output)), 'bytes': path.stat().st_size,
                  'sha256': hashlib.sha256(path.read_bytes()).hexdigest()} for path in sorted(artifacts)]
    metadata = {'schema': 1, 'platform': 'linux-amd64', 'qt_version': '6.10.3',
                'qt_kit_input': str(prefix), 'files': inventory,
                'system_dependencies': 'Linux libc, X11/XCB, Wayland and graphics system libraries'}
    (output / 'clipboard-artifact.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
