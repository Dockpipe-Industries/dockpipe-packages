"""Project only selected Apple SDK dependency metadata into private pkg-config state."""
import hashlib
from pathlib import Path
import re
import shlex
import subprocess

MODULES = {
    'libffi': ('usr/include/ffi/ffi.h', 'usr/include/ffi/ffitarget.h', 'usr/lib/libffi.tbd'),
    'zlib': ('usr/include/zlib.h', 'usr/include/zconf.h', 'usr/lib/libz.tbd'),
}
MAX_METADATA_BYTES = 65536
MAX_SDK_FILE_BYTES = 4 << 20


def read_sdk_file(sdk, relative, maximum):
    path = sdk / relative
    try:
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise ValueError('Selected SDK15.5 input is missing: ' + relative) from error
    if not resolved.is_relative_to(sdk) or not resolved.is_file():
        raise ValueError('Selected SDK15.5 input escapes its root: ' + relative)
    size = resolved.stat().st_size
    if not 0 < size <= maximum:
        raise ValueError('Selected SDK15.5 input exceeds its bound: ' + relative)
    with resolved.open('rb') as stream:
        data = stream.read(maximum + 1)
    if len(data) != size:
        raise ValueError('Selected SDK15.5 input changed while reading: ' + relative)
    return data, {'path': relative, 'resolved_path': resolved.relative_to(sdk).as_posix(),
                  'sha256': hashlib.sha256(data).hexdigest(), 'size_bytes': size}


def observe(pkgconfig, arguments, environment):
    result = subprocess.run([str(pkgconfig), *arguments], env=environment, capture_output=True,
                            text=True, timeout=15)
    if len(result.stdout) > MAX_METADATA_BYTES or len(result.stderr) > MAX_METADATA_BYTES:
        raise ValueError('Private SDK pkg-config observation exceeds its bound')
    if result.returncode:
        raise ValueError('Private SDK pkg-config graph failed: ' + result.stderr.strip())
    return result.stdout.strip()


def project(sdk_root, sdk_version, destination, pkgconfig, environment):
    sdk_root = Path(sdk_root)
    destination = Path(destination)
    if sdk_version != '15.5' or not sdk_root.is_absolute() or not sdk_root.is_dir():
        raise ValueError('Select the admitted SDK15.5 absolute root before projecting metadata')
    sdk = sdk_root.resolve(strict=True)
    if any(character in str(sdk) for character in ('\n', '\r', '\x00', '$')):
        raise ValueError('Selected SDK root cannot be represented in pkg-config metadata')
    if (not destination.is_absolute() or destination.exists()
            or destination.resolve().is_relative_to(sdk)):
        raise ValueError('SDK pkg-config projection requires a fresh private output outside the SDK')
    pkgconfig = Path(pkgconfig)
    if not pkgconfig.is_absolute() or not pkgconfig.is_file():
        raise ValueError('Use the selected absolute private pkg-config executable')
    selected = {}
    for module, required in MODULES.items():
        relative = 'usr/lib/pkgconfig/' + module + '.pc'
        data, metadata = read_sdk_file(sdk, relative, MAX_METADATA_BYTES)
        try:
            content = data.decode('utf-8')
        except UnicodeError as error:
            raise ValueError('Selected SDK metadata is not UTF-8: ' + relative) from error
        if '\x00' in content or not re.search(r'^Version\s*:\s*\S+', content, re.MULTILINE):
            raise ValueError('Selected SDK metadata has no bounded version: ' + relative)
        files = [metadata]
        for name in required:
            _, record = read_sdk_file(sdk, name, MAX_SDK_FILE_BYTES)
            files.append(record)
        # Relocate SDK /usr references only. Do not set a global pkg-config
        # sysroot: that would also rewrite the independently relocated bottles.
        escaped_sdk = str(sdk).replace('\\', '\\\\').replace(' ', '\\ ')
        content = re.sub(r'(^|[=\s]|-I|-L)/usr(?=/|\s|$)',
                         lambda match: match.group(1) + escaped_sdk + '/usr', content, flags=re.MULTILINE)
        selected[module] = {'content': content, 'source_files': files}
    destination.mkdir(mode=0o700)
    for module, record in selected.items():
        data = record['content'].encode('utf-8')
        (destination / (module + '.pc')).write_bytes(data)
        record['projected_sha256'] = hashlib.sha256(data).hexdigest()
        del record['content']
    private = dict(environment, PKG_CONFIG_PATH='', PKG_CONFIG_SYSROOT_DIR='',
                   PKG_CONFIG_LIBDIR=str(destination))
    for module, record in selected.items():
        version = observe(pkgconfig, ['--modversion', module], private)
        if not re.fullmatch(r'[0-9][A-Za-z0-9.+_-]{0,127}', version):
            raise ValueError('Selected SDK metadata has an invalid observed version: ' + module)
        record['version'] = version
    flags = observe(pkgconfig, ['--cflags', '--libs', *MODULES], private)
    for flag in shlex.split(flags):
        if flag in ('-lffi', '-lz'):
            continue
        if flag.startswith(('-I', '-L')):
            directory = Path(flag[2:])
            if directory.is_absolute() and directory.is_dir() and directory.resolve().is_relative_to(sdk):
                continue
        raise ValueError('Selected SDK metadata requests an unadmitted flag: ' + flag)
    return {'schema': 1, 'sdk_root': str(sdk), 'sdk_version': sdk_version,
            'directory': str(destination), 'modules': selected}


def admit_gio_graph(pkgconfig, environment):
    # A successful --modversion alone does not prove transitive .pc closure.
    # Require actual flags before CMake can lose include paths to a missing node.
    version = observe(pkgconfig, ['--modversion', 'gio-unix-2.0'], environment)
    if version != '2.90.1':
        raise ValueError('Private GIO pkg-config version differs from pinned2.90.1')
    flags = observe(pkgconfig, ['--cflags', '--libs', 'gio-unix-2.0'], environment)
    return {'module': 'gio-unix-2.0', 'version': version, 'flags_sha256': hashlib.sha256(flags.encode()).hexdigest()}
