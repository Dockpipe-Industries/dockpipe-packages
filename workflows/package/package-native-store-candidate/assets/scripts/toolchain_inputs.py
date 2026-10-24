"""Prepare source-bound native build tools; direct immutable archives remain separate."""
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import shlex
import subprocess
import sys
import tarfile
import zipfile

import native_inputs
import toolchain_acquire
import toolchain_linux

MAX_PROFILE = 256 << 10
VERSIONS = {'go': '1.26.10', 'cmake': '3.31.6', 'ninja': '1.12.1',
            'pkgconf-source': '2.3.0', 'meson': '1.7.2'}


def validate_reference(reference):
    if (not isinstance(reference, dict) or set(reference) != {'path', 'sha256'}
            or not isinstance(reference['sha256'], str)
            or not re.fullmatch('[0-9a-f]{64}', reference['sha256'])):
        raise ValueError('Toolchain recipe requires an exact source path and digest')
    native_inputs.relative(reference['path'])


def load_profile(source, reference, target):
    validate_reference(reference)
    path = source / reference['path']
    if (any(item.is_symlink() for item in (path, *path.parents))
            or not path.is_file() or path.stat().st_size > MAX_PROFILE
            or native_inputs.digest(path) != reference['sha256']):
        raise ValueError('Toolchain recipe differs from the bound source file')
    profile = json.loads(path.read_bytes(), object_pairs_hook=native_inputs.unique_fields)
    validate_profile(profile, target)
    return profile


def validate_profile(profile, target):
    if (not isinstance(profile, dict) or set(profile) != {'schema', 'platform', 'compiler', 'inputs', 'compiler_inputs'}
            or type(profile['schema']) is not int or profile['schema'] != 1 or profile['platform'] != target):
        raise ValueError('Unsupported source-bound toolchain profile')
    expected = {'id': 'GNU', 'version': '11.4.0', 'os_id': 'ubuntu', 'os_version': '22.04'}
    if target == 'windows-amd64':
        expected = {'id': 'MSVC', 'version': '19.51.36260.0', 'os_id': 'windows', 'os_version': '2025', 'acquisition_mode': 'hosted-exact',
                    'acquisition_sha256': profile['compiler'].get('acquisition_sha256') if isinstance(profile['compiler'], dict) else None}
        if not re.fullmatch('[0-9a-f]{64}', expected['acquisition_sha256'] or ''):
            raise ValueError('Windows toolchain requires the frozen MSVC acquisition digest')
    if target == 'darwin-arm64':
        expected = {'id': 'AppleClang', 'version': '17.0.0.17000013', 'os_id': 'macos', 'os_version': '15.0'}
    if target not in {'linux-amd64', 'windows-amd64', 'darwin-arm64'} or profile['compiler'] != expected:
        raise ValueError('Toolchain compiler differs from its explicit native baseline')
    inputs = profile['inputs']
    required = {'go', 'cmake', 'ninja', 'pkgconf-source'}
    if target == 'windows-amd64':
        required.add('meson')
    if not isinstance(inputs, list) or len(inputs) != len(required):
        raise ValueError('Toolchain recipe must contain its exact tool set')
    seen = set()
    for item in inputs:
        if not isinstance(item, dict) or item.get('id') not in required or item['id'] in seen:
            raise ValueError('Duplicate or unknown toolchain acquisition')
        seen.add(item['id'])
        if item.get('version') != VERSIONS[item['id']]:
            raise ValueError('Toolchain version differs from reviewed source profile')
        validate_asset(item)
        if item.get('root'):
            native_inputs.relative(item['root'])
        if 'destination' in item:
            native_inputs.relative(item['destination'])
        if 'license_asset' in item:
            validate_asset(item['license_asset'], binary=False)
    compiler_inputs = profile['compiler_inputs']
    if not isinstance(compiler_inputs, list) or len(compiler_inputs) != (18 if target == 'linux-amd64' else 0):
        raise ValueError('Compiler acquisition set differs from explicit native profile')
    packages = set()
    for item in compiler_inputs:
        if (not isinstance(item, dict) or item.get('package') in packages
                or not isinstance(item.get('package'), str) or not item.get('version')):
            raise ValueError('Compiler package identity is incomplete or duplicate')
        packages.add(item['package'])
        validate_asset(item)
    return profile


def validate_asset(item, binary=True):
    if not isinstance(item, dict):
        raise ValueError('Toolchain acquisition must be an object')
    toolchain_acquire.admit_url(item.get('url', ''))
    if (type(item.get('size_bytes')) is not int or not 0 < item['size_bytes'] <= toolchain_acquire.MAX_INPUT
            or not isinstance(item.get('sha256'), str) or not re.fullmatch('[0-9a-f]{64}', item['sha256'])):
        raise ValueError('Toolchain input lacks its bounded hash/size identity')
    if binary and item.get('format') not in {'tar.gz', 'tar.xz', 'zip', 'wheel', 'deb'}:
        raise ValueError('Unsupported toolchain archive format')


def unpack(entry, archive, destination):
    """Regular files only; normalize roots and bound both declared and read bytes."""
    destination.mkdir(parents=True, exist_ok=False)
    count = 0
    expanded = 0
    seen = set()

    def admit(name, size):
        nonlocal count, expanded
        name = name.rstrip('/')
        native_inputs.relative(name)
        count += 1
        expanded += size
        if count > 40000 or not 0 <= size <= 256 << 20 or expanded > 768 << 20:
            raise ValueError('Toolchain extraction exceeds its bounds')
        if name.casefold() in seen:
            raise ValueError('Duplicate or case-colliding toolchain member')
        seen.add(name.casefold())
        root = entry['root']
        if root:
            if name == root:
                return None
            if not name.startswith(root + '/'):
                raise ValueError('Toolchain archive root differs from recipe')
            name = name[len(root) + 1:]
        return destination / name

    def write(target, stream, size, mode):
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open('xb') as output:
            remaining = size
            while remaining:
                block = stream.read(min(1 << 20, remaining))
                if not block:
                    raise ValueError('Truncated toolchain member')
                output.write(block)
                remaining -= len(block)
            if stream.read(1):
                raise ValueError('Toolchain member exceeds declared size')
        target.chmod(0o755 if mode & 0o111 else 0o644)

    if entry['format'] in {'zip', 'wheel'}:
        with zipfile.ZipFile(archive) as source:
            for item in source.infolist():
                target = admit(item.filename, item.file_size)
                mode = item.external_attr >> 16
                if mode & 0o170000 not in {0, 0o040000, 0o100000}:
                    raise ValueError('Toolchain ZIP links or special files are unsupported')
                if target is None:
                    continue
                if item.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    with source.open(item) as stream:
                        write(target, stream, item.file_size, mode)
    else:
        with tarfile.open(archive, tarinfo=native_inputs.BoundedTarInfo) as source:
            for item in source:
                target = admit(item.name, item.size)
                if not item.isfile() and not item.isdir():
                    raise ValueError('Toolchain tar links or special files are unsupported')
                if target is None:
                    continue
                if item.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    with source.extractfile(item) as stream:
                        write(target, stream, item.size, item.mode)
                    os.utime(target, (item.mtime, item.mtime))


def run(arguments, environment, directory=None):
    completed = subprocess.run([str(item) for item in arguments], cwd=directory, env=environment,
                               capture_output=True, text=True, timeout=300)
    if completed.returncode:
        raise ValueError('Native toolchain command failed: ' + str(arguments[0]) + '\n'
                         + (completed.stdout + completed.stderr)[-4096:])
    if len(completed.stdout) > 1 << 20:
        raise ValueError('Native toolchain command output exceeds capacity')
    return completed.stdout.strip()


def merge_tree(source, destination):
    for path in sorted(source.rglob('*')):
        if path.is_symlink():
            raise ValueError('Prepared tool input contains an unexpected link')
        target = destination / path.relative_to(source)
        if path.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        elif target.exists():
            raise ValueError('Toolchain inputs overlap; refusing replacement')
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)


def prepare_windows(profile, prefix, cache, acquire, environment):
    prefix.mkdir(parents=True, exist_ok=False)
    environment = native_inputs.compiler_environment({'compiler': profile['compiler']}, prefix, environment)
    tools = {item['id']: item for item in profile['inputs']}
    for name in ('go', 'cmake', 'ninja', 'pkgconf-source'):
        entry = tools[name]
        staging = prefix / ('input-' + name)
        unpack(entry, acquire(entry), staging)
        destination = prefix / ('bin' if name == 'ninja' else 'pkgconf-source' if name == 'pkgconf-source' else '')
        merge_tree(staging, destination)
        shutil.rmtree(staging)
    wheel = tools['meson']
    wheel_path = prefix / Path(wheel['url']).name
    shutil.copyfile(acquire(wheel), wheel_path)
    environment['PATH'] = str(prefix / 'bin') + os.pathsep + environment['PATH']
    venv = prefix / 'meson-venv'
    run([sys.executable, '-m', 'venv', str(venv)], environment)
    python = venv / 'Scripts/python.exe'
    run([python, '-m', 'pip', 'install', '--no-index', '--no-deps', '--no-cache-dir', wheel_path], environment)
    meson = [python, '-m', 'mesonbuild.mesonmain']
    build = prefix / 'pkgconf-build'
    run([*meson, 'setup', build, prefix / 'pkgconf-source', '--backend=ninja', '--buildtype=release',
         '--prefix=' + str(prefix / 'pkgconf'), '-Dtests=disabled', '-Ddefault_library=static',
         '-Dwith-system-libdir=', '-Dwith-system-includedir='], environment)
    run([*meson, 'compile', '-C', build, '-j', '2'], environment)
    run([*meson, 'install', '-C', build], environment)
    shutil.copyfile(prefix / 'pkgconf/bin/pkgconf.exe', prefix / 'bin/pkg-config.exe')
    environment['GOROOT'] = str(prefix)
    return environment


def prepare_macos(profile, prefix, acquire, environment):
    """Use official tools and the explicitly selected native Xcode; never change selection."""
    import framework_extract
    developer = Path('/Applications/Xcode_16.4.app/Contents/Developer')
    if environment.get('DEVELOPER_DIR') != str(developer) or not developer.is_dir():
        raise ValueError('Select the job-local Xcode16.4 developer directory explicitly')
    sdk = run(['/usr/bin/xcrun', '--sdk', 'macosx', '--show-sdk-path'], environment)
    if run(['/usr/bin/xcrun', '--sdk', 'macosx', '--show-sdk-version'], environment) != '15.5':
        raise ValueError('Native Mac toolchain requires the exact SDK15.5 baseline')
    if not Path(sdk).is_absolute() or not Path(sdk).is_dir():
        raise ValueError('Selected macOS SDK path is unavailable')
    compilers = {}
    for name, command in (('CC', 'clang'), ('CXX', 'clang++')):
        binary = Path(run(['/usr/bin/xcrun', '--find', command], environment))
        if not binary.is_file() or not binary.resolve().is_relative_to(developer.resolve()):
            raise ValueError('Native Mac compiler escaped the explicitly selected Xcode')
        compilers[name] = str(binary)
    environment = dict(environment, **compilers, SDKROOT=sdk, MACOSX_DEPLOYMENT_TARGET='15.0')
    prefix.mkdir(parents=True, exist_ok=False)
    inputs = {entry['id']: entry for entry in profile['inputs']}
    for name in ('go', 'ninja', 'pkgconf-source'):
        unpack(inputs[name], acquire(inputs[name]), prefix / inputs[name]['destination'])
    cmake = inputs['cmake']
    framework_extract.extract_locked([{'path': acquire(cmake), 'sha256': cmake['sha256'],
        'size_bytes': cmake['size_bytes']}], prefix / 'cmake')
    binary_directory = prefix / 'bin'
    binary_directory.mkdir()
    app = Path('cmake') / cmake['root'] / 'CMake.app/Contents/bin'
    for name, target in {'go': Path('go/bin/go'), 'gofmt': Path('go/bin/gofmt'),
                         'cmake': app / 'cmake', 'ctest': app / 'ctest',
                         'ninja': Path('ninja/ninja')}.items():
        source = prefix / target
        if not source.is_file() or not source.resolve().is_relative_to(prefix.resolve()):
            raise ValueError('Official Mac archive lacks its contained tool entrypoint')
        (binary_directory / name).symlink_to(Path('..') / target)
    environment['PATH'] = str(binary_directory) + os.pathsep + '/usr/bin:/bin:/usr/sbin:/sbin'
    environment['GOROOT'] = str(prefix / 'go')
    environment['CFLAGS'] = '-O2 -arch arm64 -mmacosx-version-min=15.0 -isysroot ' + shlex.quote(sdk)
    source = prefix / 'pkgconf-source'
    run([source / 'configure', '--prefix=' + str(prefix / 'pkgconf'), '--disable-shared', '--enable-static',
         '--with-pkg-config-dir=', '--with-system-libdir=', '--with-system-includedir='], environment, source)
    run(['/usr/bin/make', '-j2'], environment, source)
    run(['/usr/bin/make', 'install'], environment, source)
    binary = prefix / 'pkgconf/bin/pkgconf'
    if not binary.is_file() or binary.is_symlink():
        raise ValueError('Native Mac pkgconf build did not produce its declared executable')
    (binary_directory / 'pkg-config').symlink_to('../pkgconf/bin/pkgconf')
    return environment


def probe(prefix, work, target, environment):
    work.mkdir(exist_ok=False)
    extension = '.exe' if target.startswith('windows-') else ''
    versions = {}
    for name, expected in [('go', 'go1.26.10'), ('cmake', '3.31.6'), ('ninja', '1.12.1'), ('pkg-config', '2.3.0')]:
        binary = prefix / 'bin' / (name + extension)
        output = run([binary, 'version' if name == 'go' else '--version'], environment)
        if expected not in output.splitlines()[0]:
            raise ValueError('Prepared native tool version differs: ' + name)
        versions[name] = {'version': output.splitlines()[0], 'sha256': native_inputs.digest(binary)}
    source = work / 'source'
    source.mkdir()
    windows = target == 'windows-amd64'
    declaration = '__declspec(dllexport)' if windows else ''
    library_source = """#include <string>

extern "C" @DECLARATION@ std::string native_value() {
    return "native-abi-proof";
}
"""
    (source / 'library.cpp').write_text(library_source.replace('@DECLARATION@', declaration))
    assertion = '_MSC_VER == 1951 && _MSC_FULL_VER == 195136260' if windows else '__GNUC__ == 11 && __GNUC_MINOR__ == 4'
    if target == 'darwin-arm64':
        assertion = '__clang_major__ == 17'
    runtime_check = '#ifndef _DLL\n#error Dynamic MSVC runtime required\n#endif\n' if windows else 'static_assert(_GLIBCXX_USE_CXX11_ABI == 1);\n'
    if target == 'darwin-arm64':
        runtime_check = '#ifndef __APPLE__\n#error Native Apple compiler required\n#endif\n'
    if windows:
        runtime_check += r"""
#include <windows.h>
static bool runtime_is_app_local() {
    wchar_t executable[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;
    std::wstring directory(executable, length);
    directory.resize(directory.find_last_of(L"\\/") + 1);
    const wchar_t* names[] = {
        L"msvcp140.dll",
        L"vcruntime140.dll",
        L"vcruntime140_1.dll",
        L"vcruntime140_threads.dll",
        L"concrt140.dll",
        L"msvcp140_1.dll",
        L"msvcp140_2.dll",
        L"msvcp140_atomic_wait.dll",
        L"msvcp140_codecvt_ids.dll",
        L"vccorlib140.dll"
    };
    for (const wchar_t* name : names) {
        HMODULE module = GetModuleHandleW(name);
        if (!module)
            continue;
        wchar_t path[MAX_PATH];
        length = GetModuleFileNameW(module, path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH ||
            _wcsicmp(path, (directory + name).c_str()) != 0)
            return false;
    }
    return GetModuleHandleW(L"msvcp140.dll") != nullptr;
}
"""
    main_source = """#include <string>
@RUNTIME_CHECK@
static_assert(@ASSERTION@);
static_assert(sizeof(void*) == 8);

extern "C" std::string native_value();

int main() {
    return @RUNTIME_CONDITION@native_value() == "native-abi-proof" ? 0 : 1;
}
"""
    main_source = main_source.replace('@RUNTIME_CHECK@', runtime_check)
    main_source = main_source.replace('@ASSERTION@', assertion)
    main_source = main_source.replace('@RUNTIME_CONDITION@', 'runtime_is_app_local() && ' if windows else '')
    (source / 'main.cpp').write_text(main_source)
    (source / 'CMakeLists.txt').write_text("""cmake_minimum_required(VERSION 3.24)
project(NativeABI LANGUAGES CXX)
file(WRITE "${CMAKE_BINARY_DIR}/identity.json" "{\\"id\\":\\"${CMAKE_CXX_COMPILER_ID}\\",\\"version\\":\\"${CMAKE_CXX_COMPILER_VERSION}\\"}")

add_library(native-contract SHARED library.cpp)
add_executable(native-proof main.cpp)
target_link_libraries(native-proof PRIVATE native-contract)
target_compile_features(native-proof PRIVATE cxx_std_17)
""")
    build = work / 'build'
    configure = [prefix / 'bin' / ('cmake' + extension), '-S', source, '-B', build, '-G', 'Ninja',
                 '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_COMPILER=' + environment['CXX'],
                 '-DCMAKE_MAKE_PROGRAM=' + str(prefix / 'bin' / ('ninja' + extension))]
    if target == 'darwin-arm64':
        configure += ['-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0',
                      '-DCMAKE_OSX_SYSROOT=' + environment['SDKROOT']]
    run(configure, environment)
    run([prefix / 'bin' / ('cmake' + extension), '--build', build, '--parallel', '2'], environment)
    if windows:
        import windows_runtime
        runtime_receipt = json.loads(Path(environment['DOCKPIPE_NATIVE_MSVC_RECEIPT']).read_bytes())['host_runtime']
        acquisition = Path(__file__).resolve().parents[5] / 'release/native-compiler/windows-hosted-19.51.json'
        expected_runtime = json.loads(acquisition.read_bytes())['desktop_runtime']
        windows_runtime.stage_probe_runtime(environment['DOCKPIPE_NATIVE_MSVC_ROOT'], runtime_receipt,
                                            expected_runtime, build)
    run([build / ('native-proof' + extension)], environment)
    if target == 'darwin-arm64':
        identity = json.loads((build / 'identity.json').read_text())
        if identity != {'id': 'AppleClang', 'version': '17.0.0.17000013'}:
            raise ValueError('Native Mac toolchain compiler differs from the exact AppleClang17 baseline')
        if 'libc++.1.dylib' not in run(['/usr/bin/otool', '-L', build / 'native-proof'], environment):
            raise ValueError('Native Mac shared ABI fixture lacks the expected system libc++ dependency')
    if target == 'linux-amd64':
        if platform.libc_ver()[1] != '2.35':
            raise ValueError('Native toolchain ABI probe requires glibc2.35')
        if 'libstdc++.so.6' not in run(['/usr/bin/readelf', '--dynamic', build / 'native-proof'], environment):
            raise ValueError('GNU ABI probe unexpectedly selected a static runtime fallback')
    metadata = work / 'pkgconfig'
    metadata.mkdir()
    (metadata / 'native-proof.pc').write_text('Name: native-proof\nDescription: private probe\nVersion: 1\nCflags: -DNATIVE_PREFIX_PROOF=1\n')
    isolated = dict(environment, PKG_CONFIG_PATH='', PKG_CONFIG_LIBDIR=str(metadata))
    if run([prefix / 'bin' / ('pkg-config' + extension), '--cflags', 'native-proof'], isolated) != '-DNATIVE_PREFIX_PROOF=1':
        raise ValueError('Prepared pkgconf failed isolated metadata resolution')
    return versions


def prepare(source, reference, target, prefix, cache, fetch, environment):
    profile = load_profile(source, reference, target)
    environment = {key: value for key, value in environment.items()
                   if not key.startswith('PIP_') and key not in {'PYTHONHOME', 'PYTHONPATH', 'VIRTUAL_ENV',
                       'CMAKE_TOOLCHAIN_FILE', 'CMAKE_PREFIX_PATH', 'GH_TOKEN', 'GITHUB_TOKEN',
                       'GH_ENTERPRISE_TOKEN', 'GITHUB_ENTERPRISE_TOKEN'}}
    environment.update(PIP_CONFIG_FILE=os.devnull, PYTHONNOUSERSITE='1')
    expected_system = {'windows-amd64': 'Windows', 'linux-amd64': 'Linux', 'darwin-arm64': 'Darwin'}[target]
    if platform.system() != expected_system or prefix.exists():
        raise ValueError('Toolchain preparation requires its native host and fresh prefix')
    if target == 'linux-amd64':
        release = platform.freedesktop_os_release()
        if release.get('ID') != 'ubuntu' or release.get('VERSION_ID') != '22.04':
            raise ValueError('Toolchain preparation requires Ubuntu22.04; no host fallback')
    if target == 'darwin-arm64' and (platform.machine() != 'arm64' or platform.mac_ver()[0].split('.')[0] != '15'):
        raise ValueError('Mac toolchain preparation requires its native arm64/macOS15 baseline')
    def acquire(entry):
        path = cache / entry['sha256']
        if not path.exists() and not fetch:
            raise ValueError('Toolchain input is not cached and fetching is disabled')
        return toolchain_acquire.acquire(entry, cache)
    for entry in profile['inputs'] + profile['compiler_inputs']:
        acquire(entry)
        if 'license_asset' in entry:
            acquire(entry['license_asset'])
    if target == 'windows-amd64':
        environment = prepare_windows(profile, prefix, cache, acquire, environment)
    elif target == 'darwin-arm64':
        environment = prepare_macos(profile, prefix, acquire, environment)
    else:
        manifests = prefix.with_name(prefix.name + '-manifests')
        manifests.mkdir(exist_ok=False)
        recipe = manifests / 'linux-inputs.json'
        recipe.write_text(json.dumps(profile['inputs']))
        (manifests / 'gcc-profile-acquisitions.json').write_text(json.dumps(profile['compiler_inputs']))
        toolchain_linux.prepare(recipe, cache, prefix)
        environment = native_inputs.compiler_environment({'compiler': profile['compiler']}, prefix, environment)
        environment['LD_LIBRARY_PATH'] = str(prefix / 'compiler/usr/lib/x86_64-linux-gnu')
        environment['GOROOT'] = str(prefix / 'go')
    license_root = prefix / 'licenses'
    license_root.mkdir(exist_ok=True)
    for entry in profile['inputs']:
        if 'license_asset' in entry:
            shutil.copyfile(acquire(entry['license_asset']), license_root / (entry['id'] + '-COPYING'))
    versions = probe(prefix, prefix.with_name(prefix.name + '-probe'), target, environment)
    files = []
    for path in sorted(prefix.rglob('*')):
        if path.is_symlink():
            if not path.exists() or not path.resolve().is_relative_to(prefix.resolve()):
                raise ValueError('Toolchain prefix has an escaping or missing link')
        elif path.is_file():
            files.append({'path': path.relative_to(prefix).as_posix(), 'sha256': native_inputs.digest(path),
                          'size_bytes': path.stat().st_size})
    if len(files) > 80000:
        raise ValueError('Toolchain inventory exceeds its bound')
    licenses = [item for item in files if any(part.lower().startswith(('license', 'copyright', 'copying'))
                                             for part in Path(item['path']).parts)]
    if not licenses:
        raise ValueError('Prepared toolchain has no retained license inventory')
    receipt = {'schema': 1, 'platform': target, 'profile': reference, 'tools': versions,
               'compiler': profile['compiler'], 'acquisitions': profile['inputs'] + profile['compiler_inputs'],
               'inventory': files, 'licenses': licenses, 'system_installs_performed': False,
               'checks': ['exact-tool-versions', 'native-shared-library-consumer', 'isolated-pkgconf-metadata']}
    if target == 'windows-amd64':
        receipt['compiler_receipt_sha256'] = native_inputs.digest(Path(environment['DOCKPIPE_NATIVE_MSVC_RECEIPT']))
        receipt['sdk_tools'] = {name: native_inputs.digest(Path(environment['DOCKPIPE_NATIVE_' + name.upper()]))
                                for name in ('rc', 'mt')}
    if target == 'darwin-arm64':
        receipt['native_sdk'] = {'xcode_version': '16.4', 'sdk_version': '15.5', 'deployment_target': '15.0',
                                 'compilers': {name: native_inputs.digest(Path(environment[name])) for name in ('CC', 'CXX')}}
    receipt_path = prefix.with_name(prefix.name + '-receipt.json')
    receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
    return prefix, environment, receipt_path


def validate_receipt(receipt, target, reference):
    required = {'schema', 'platform', 'profile', 'tools', 'compiler', 'acquisitions', 'inventory', 'licenses',
                'system_installs_performed', 'checks'}
    if target == 'windows-amd64':
        required |= {'compiler_receipt_sha256', 'sdk_tools'}
    if target == 'darwin-arm64':
        required.add('native_sdk')
    if (not isinstance(receipt, dict) or set(receipt) != required or type(receipt['schema']) is not int
            or receipt['schema'] != 1 or receipt['platform'] != target or receipt['profile'] != reference
            or receipt['system_installs_performed'] is not False
            or receipt['checks'] != ['exact-tool-versions', 'native-shared-library-consumer', 'isolated-pkgconf-metadata']):
        raise ValueError('Native toolchain receipt differs from its source-bound preparation')
    acquisitions = receipt['acquisitions']
    if not isinstance(acquisitions, list) or len(acquisitions) > 32 or any(not isinstance(item, dict) for item in acquisitions):
        raise ValueError('Toolchain receipt acquisition inventory is invalid')
    validate_profile({'schema': 1, 'platform': target, 'compiler': receipt['compiler'],
        'inputs': [item for item in acquisitions if 'id' in item],
        'compiler_inputs': [item for item in acquisitions if 'package' in item]}, target)
    tools = receipt['tools']
    if not isinstance(tools, dict) or set(tools) != {'go', 'cmake', 'ninja', 'pkg-config'}:
        raise ValueError('Toolchain receipt lacks its exact executed tool inventory')
    for name, item in tools.items():
        if (not isinstance(item, dict) or set(item) != {'version', 'sha256'}
                or not isinstance(item['version'], str) or not 0 < len(item['version']) <= 512
                or any(value in item['version'] for value in ('\r', '\n'))
                or not isinstance(item['sha256'], str) or not re.fullmatch('[0-9a-f]{64}', item['sha256'])):
            raise ValueError('Toolchain receipt tool identity is invalid')
        expected = {'go': 'go1.26.10', 'cmake': '3.31.6', 'ninja': '1.12.1', 'pkg-config': '2.3.0'}[name]
        if expected not in item['version']:
            raise ValueError('Toolchain receipt tool version differs')
    records = receipt['inventory']
    if not isinstance(records, list) or not 0 < len(records) <= 80000:
        raise ValueError('Toolchain receipt file inventory exceeds its bound')
    files = {}
    for item in records:
        if not isinstance(item, dict) or set(item) != {'path', 'sha256', 'size_bytes'}:
            raise ValueError('Toolchain receipt file identity is invalid')
        native_inputs.relative(item['path'])
        if (item['path'] in files or type(item['size_bytes']) is not int or not 0 <= item['size_bytes'] <= 256 << 20
                or not isinstance(item['sha256'], str) or not re.fullmatch('[0-9a-f]{64}', item['sha256'])):
            raise ValueError('Toolchain receipt contains duplicate or invalid files')
        files[item['path']] = item
    licenses = receipt['licenses']
    if (not isinstance(licenses, list) or not 0 < len(licenses) <= len(records)
            or any(not isinstance(item, dict) or files.get(item.get('path')) != item for item in licenses)):
        raise ValueError('Toolchain receipt licenses differ from its exact prefix inventory')
    if target == 'windows-amd64':
        if (not isinstance(receipt['compiler_receipt_sha256'], str)
                or not re.fullmatch('[0-9a-f]{64}', receipt['compiler_receipt_sha256'])
                or not isinstance(receipt['sdk_tools'], dict) or set(receipt['sdk_tools']) != {'rc', 'mt'}
                or any(not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value)
                       for value in receipt['sdk_tools'].values())):
            raise ValueError('Toolchain receipt lacks its prepared Microsoft compiler/SDK identities')
    if target == 'darwin-arm64':
        sdk = receipt['native_sdk']
        if (not isinstance(sdk, dict) or set(sdk) != {'xcode_version', 'sdk_version', 'deployment_target', 'compilers'}
                or sdk['xcode_version'] != '16.4' or sdk['sdk_version'] != '15.5' or sdk['deployment_target'] != '15.0'
                or not isinstance(sdk['compilers'], dict) or set(sdk['compilers']) != {'CC', 'CXX'}
                or any(not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value)
                       for value in sdk['compilers'].values())):
            raise ValueError('Native Mac toolchain receipt differs from the selected compiler/SDK baseline')
    return receipt
