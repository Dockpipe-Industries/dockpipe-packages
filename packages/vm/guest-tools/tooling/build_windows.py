#!/usr/bin/env python3
"""Build, test and inventory the native Windows Guest Tools with a locked Qt kit."""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def selected_directory(name):
    value = os.environ.get(name)
    if not value or not Path(value).is_absolute():
        raise ValueError(name + ' must select an explicit absolute task directory')
    return Path(value).resolve()


def run(arguments, environment):
    subprocess.run([str(argument) for argument in arguments], env=environment, check=True, timeout=300)


def main():
    if platform.system() != 'Windows':
        raise ValueError('Guest Tools must build and test on a native Windows host')
    qt = selected_directory('DOCKPIPE_GUEST_TOOLS_QT')
    output = selected_directory('DOCKPIPE_GUEST_TOOLS_OUTPUT')
    kit = qt / '6.10.3/mingw_64'
    compiler = qt / 'Tools/mingw1310_64/bin'
    cmake = qt / 'Tools/CMake_64/bin/cmake.exe'
    ctest = qt / 'Tools/CMake_64/bin/ctest.exe'
    ninja = qt / 'Tools/Ninja/ninja.exe'
    lock_path = ROOT / 'tooling/toolchain.lock.json'
    receipt = json.loads((qt / 'toolchain-receipt.json').read_text())
    if receipt.get('lock_sha256') != hashlib.sha256(lock_path.read_bytes()).hexdigest():
        raise ValueError('The selected toolchain does not match the committed artifact lock')
    environment = dict(os.environ)
    environment['PATH'] = str(compiler) + os.pathsep + str(kit / 'bin') + os.pathsep + environment['PATH']
    actual_qt = subprocess.check_output([str(kit / 'bin/qmake.exe'), '-query', 'QT_VERSION'], env=environment, text=True).strip()
    actual_compiler = subprocess.check_output([str(compiler / 'g++.exe'), '-dumpfullversion'], env=environment, text=True).strip()
    if actual_qt != '6.10.3' or actual_compiler != '13.1.0':
        raise ValueError('Guest Tools requires Qt 6.10.3 and the locked MinGW 13.1.0 compiler')
    if output.exists():
        raise ValueError('Select a fresh build output directory; previous evidence is preserved')
    output.mkdir(parents=True)
    build = output / 'build'
    run([cmake, '-S', ROOT, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
         '-DCMAKE_PREFIX_PATH=' + str(kit), '-DCMAKE_MAKE_PROGRAM=' + str(ninja),
         '-DCMAKE_C_COMPILER=' + str(compiler / 'gcc.exe'),
         '-DCMAKE_CXX_COMPILER=' + str(compiler / 'g++.exe')], environment)
    run([cmake, '--build', build, '--parallel', '2'], environment)
    run([ctest, '--test-dir', build, '--output-on-failure', '--no-tests=error'], environment)
    bundle = output / 'bundle'
    bundle.mkdir()
    executable = bundle / 'dockpipe-guest-tools.exe'
    shutil.copy2(build / executable.name, executable)
    run([kit / 'bin/windeployqt.exe', '--release', '--compiler-runtime', '--no-opengl-sw',
         '--no-translations', '--no-system-d3d-compiler', '--dir', bundle, executable], environment)
    shutil.copy2(ROOT / 'assets/scripts/install-guest-tools.ps1', bundle / 'install-guest-tools.ps1')
    shutil.copytree(ROOT / 'licenses', bundle / 'licenses')
    shutil.copy2(ROOT.parents[2] / 'LICENSE', bundle / 'licenses/Dockpipe-Apache-2.0.txt')
    for name in ('gcc', 'mingw-w64', 'winpthreads'):
        shutil.copytree(qt / 'Tools/mingw1310_64/licenses' / name, bundle / 'licenses/mingw' / name)
    inventory = []
    for path in sorted(bundle.rglob('*')):
        if path.is_symlink():
            raise ValueError('Guest Tools bundle must contain regular files only')
        if path.is_file():
            if path.relative_to(bundle).parts[0] != 'licenses' and path.suffix.lower() in ('.pdb', '.obj', '.lib', '.a', '.o', '.ilk'):
                raise ValueError('Guest Tools bundle contains a private build artifact')
            inventory.append({'path': path.relative_to(bundle).as_posix(), 'bytes': path.stat().st_size,
                              'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    document = {'schema': 1, 'product': 'DockpipeGuestTools', 'version': '1.0.0',
                'platform': 'windows-amd64', 'qt_version': '6.10.3', 'files': inventory}
    (bundle / 'inventory.json').write_text(json.dumps(document, indent=2) + '\n')
    artifact = output / 'dockpipe-guest-tools-1.0.0-windows-amd64.zip'
    with zipfile.ZipFile(artifact, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(bundle.rglob('*')):
            if path.is_file():
                entry = zipfile.ZipInfo(path.relative_to(bundle).as_posix(), date_time=(1980, 1, 1, 0, 0, 0))
                entry.compress_type = zipfile.ZIP_DEFLATED
                entry.external_attr = 0o100644 << 16
                archive.writestr(entry, path.read_bytes())
    source_inventory = []
    for path in sorted(ROOT.rglob('*')):
        if path.is_file() and '__pycache__' not in path.parts:
            source_inventory.append({'path': path.relative_to(ROOT).as_posix(),
                                     'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    descriptor = {
        'schema': 1,
        'product': 'DockpipeGuestTools',
        'version': '1.0.0',
        'platform': 'windows-amd64',
        'qt_version': '6.10.3',
        'entrypoint': 'dockpipe-guest-tools.exe',
        'installer': 'install-guest-tools.ps1',
        'media_subdirectory': 'GuestTools',
        'archive': {'name': artifact.name, 'size_bytes': artifact.stat().st_size,
                    'sha256': hashlib.sha256(artifact.read_bytes()).hexdigest()},
        'inventory_sha256': hashlib.sha256((bundle / 'inventory.json').read_bytes()).hexdigest(),
        'source_inventory_sha256': hashlib.sha256(json.dumps(
            source_inventory, separators=(',', ':')).encode()).hexdigest(),
    }
    (output / 'guest-tools-release.json').write_text(json.dumps(descriptor, indent=2) + '\n')
    print('Built and verified native Windows Guest Tools: ' + str(bundle), flush=True)
    print('Immutable delivery archive and descriptor: ' + str(artifact), flush=True)


if __name__ == '__main__':
    main()
