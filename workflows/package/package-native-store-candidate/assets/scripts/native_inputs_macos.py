"""Proposed macOS 15 native recipe adapter. No global Homebrew installation or service changes."""
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile
import time
import urllib.parse
import urllib.request

import framework_extract
import macho_metadata
import private_pkgconfig

BASELINE = {'id': 'AppleClang', 'version': '17.0.0.17000013', 'os_id': 'macos', 'os_version': '15.0'}
PATCH_SHA256 = '69a03882576fd264a287a21d3dd298638e8fd58515552acb645edff6ab90526b'
PACKAGES = {'glib', 'gettext', 'pcre2', 'json-c', 'libunistring', 'libffi', 'zlib'}
MAX_ASSET = 256 << 20


def digest(path):
    checksum = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            checksum.update(block)
    return checksum.hexdigest()


def validate(recipe):
    if (not isinstance(recipe, dict) or set(recipe) != {'schema', 'platform', 'compiler', 'archives', 'licenses', 'libraries'}
            or type(recipe['schema']) is not int or recipe['schema'] != 1
            or recipe['platform'] != 'darwin-arm64' or recipe['compiler'] != BASELINE):
        raise ValueError('The macOS recipe requires its explicit OS15/arm64/compiler17 baseline')
    entries = recipe['archives']
    if not isinstance(entries, list) or len(entries) != 11:
        raise ValueError('The complete eleven-archive Mac recipe is required')
    expected_records = {
        'qtbase': ('qt-qtbase', '6.10.3', 'qt/6.10.3/macos', ''),
        'qttools': ('qt-qttools', '6.10.3', 'qt/6.10.3/macos', ''),
        'glib-source': ('glib', '2.90.1', 'gio/license-source', 'glib-2.90.1'),
        'dbus-source': ('dbus', '1.16.2', 'broker-source', 'dbus-1.16.2'),
    }
    for package, version in {'glib': '2.90.1', 'gettext': '1.0', 'pcre2': '10.49', 'json-c': '0.19', 'libunistring': '1.5', 'libffi': '3.8.0', 'zlib': '1.3.2'}.items():
        expected_records[package] = (package, version, 'gio/Cellar', '')
    seen = set()
    total = 0
    for entry in entries:
        if (not isinstance(entry, dict) or set(entry) != {'id', 'destination', 'asset', 'package', 'version'}
                or not isinstance(entry['id'], str) or entry['id'] in seen):
            raise ValueError('Invalid or duplicate macOS archive record')
        seen.add(entry['id'])
        framework_extract.name_parts(entry['destination'])
        asset = entry['asset']
        if not isinstance(asset, dict):
            raise ValueError('Mac archive asset must be an immutable descriptor')
        observed = (entry['package'], entry['version'], entry['destination'], asset.get('root'))
        if expected_records.get(entry['id']) != observed:
            raise ValueError('Mac archive package/version/output role differs from its reviewed baseline')
        if (not {'url', 'sha256', 'size_bytes', 'format', 'root'}.issubset(asset)
                or set(asset) - {'url', 'sha256', 'size_bytes', 'format', 'root', 'redirect_hosts'}
                or not isinstance(asset['sha256'], str) or not re.fullmatch('[0-9a-f]{64}', asset['sha256'])
                or type(asset['size_bytes']) is not int or not 0 < asset['size_bytes'] <= MAX_ASSET):
            raise ValueError('Mac input requires an exact bounded public hash/size')
        if asset['root']:
            framework_extract.name_parts(asset['root'])
        mirrors = asset.get('redirect_hosts', [])
        if (not isinstance(mirrors, list) or len(mirrors) > 64 or len(set(mirrors)) != len(mirrors)
                or any(not isinstance(host, str) or not re.fullmatch(r'[a-z0-9-]+(?:\.[a-z0-9-]+)+', host)
                       or host.endswith(('.local', '.internal', '.localhost')) for host in mirrors)):
            raise ValueError('Mac Qt mirrors require exact public Metalink hostnames')
        url = urllib.parse.urlsplit(asset['url'])
        if mirrors and url.hostname != 'download.qt.io':
            raise ValueError('Only Qt Metalink snapshots may extend redirect hosts')
        allowed = (url.netloc == 'download.qt.io' and url.path.startswith('/online/qtsdkrepository/') and asset['format'] == '7z'
                   or url.netloc == 'download.gnome.org' and url.path == '/sources/glib/2.90/glib-2.90.1.tar.xz' and asset['format'] == 'tar.xz'
                   or url.netloc == 'dbus.freedesktop.org' and url.path == '/releases/dbus/dbus-1.16.2.tar.xz' and asset['format'] == 'tar.xz')
        if url.netloc == 'ghcr.io':
            allowed = (entry['package'] in PACKAGES and asset['format'] == 'tar.gz'
                       and url.path == '/v2/homebrew/core/' + entry['package'] + '/blobs/sha256:' + asset['sha256'])
        if url.scheme != 'https' or url.query or url.fragment or not allowed:
            raise ValueError('Mac archive must use its exact official public origin or OCI digest')
        total += asset['size_bytes']
    if seen != {'qtbase', 'qttools', 'glib-source', 'dbus-source'} | PACKAGES or total > 512 << 20:
        raise ValueError('Mac archive identities or aggregate capacity differ')
    for key, maximum in (('licenses', 256), ('libraries', 64)):
        values = recipe[key]
        if not isinstance(values, list) or not 1 <= len(values) <= maximum:
            raise ValueError('Mac evidence inventory exceeds its bound')
        paths = set()
        for entry in values:
            if not isinstance(entry, dict) or set(entry) != {'path', 'sha256', 'size_bytes', 'package', 'version', 'archive_id'}:
                raise ValueError('Mac file evidence requires its source archive identity')
            framework_extract.name_parts(entry['path'])
            if (entry['path'].casefold() in paths or not isinstance(entry['archive_id'], str) or entry['archive_id'] not in seen
                    or not isinstance(entry['sha256'], str) or not re.fullmatch('[0-9a-f]{64}', entry['sha256'])
                    or type(entry['size_bytes']) is not int or not 0 < entry['size_bytes'] <= MAX_ASSET):
                raise ValueError('Mac file evidence is duplicated or invalid')
            owner = next(record for record in entries if record['id'] == entry['archive_id'])
            if entry['package'] != owner['package'] or entry['version'] != owner['version']:
                raise ValueError('Mac file evidence owner differs from its declared public archive')
            paths.add(entry['path'].casefold())
    return recipe


class AnonymousOCIRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file_pointer, code, message, headers, new_url):
        parsed = urllib.parse.urlsplit(new_url)
        if parsed.scheme != 'https' or parsed.hostname not in {'ghcr.io', 'pkg-containers.githubusercontent.com'}:
            raise ValueError('Anonymous OCI download redirected outside its public HTTPS hosts')
        result = super().redirect_request(request, file_pointer, code, message, headers, new_url)
        result.remove_header('Authorization')
        return result


def acquire_oci(entry, cache, fetch=False, opener=None):
    asset = entry['asset']
    if (not isinstance(asset.get('sha256'), str) or not re.fullmatch('[0-9a-f]{64}', asset['sha256'])
            or type(asset.get('size_bytes')) is not int or not 0 < asset['size_bytes'] <= MAX_ASSET
            or asset.get('format') != 'tar.gz' or asset.get('root') != ''):
        raise ValueError('OCI acquisition requires a bounded immutable bottle descriptor')
    expected = 'https://ghcr.io/v2/homebrew/core/' + entry['package'] + '/blobs/sha256:' + asset['sha256']
    if entry['package'] not in PACKAGES or asset['url'] != expected:
        raise ValueError('Only pinned public Homebrew bottle digests may use anonymous OCI')
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / asset['sha256']
    if target.is_symlink():
        raise ValueError('Content-addressed bottle cache cannot contain a symlink')
    if not target.exists():
        if not fetch:
            raise ValueError('Locked OCI bottle is absent from the explicit cache')
        opener = opener or urllib.request.build_opener(AnonymousOCIRedirect())
        scope = urllib.parse.quote('repository:homebrew/core/' + entry['package'] + ':pull', safe='')
        with opener.open('https://ghcr.io/token?service=ghcr.io&scope=' + scope, timeout=30) as response:
            payload = response.read(16385)
        if len(payload) > 16384:
            raise ValueError('Anonymous OCI token metadata exceeds its bound')
        token = json.loads(payload).get('token')
        if not isinstance(token, str) or not 0 < len(token) <= 8192:
            raise ValueError('Public OCI registry did not issue a bounded anonymous pull token')
        request = urllib.request.Request(expected, headers={'Authorization': 'Bearer ' + token})
        # No user token/environment credentials are read, stored or logged.
        part = target.with_suffix('.part')
        remaining = asset['size_bytes']
        started = time.monotonic()
        try:
            with opener.open(request, timeout=60) as response, part.open('xb') as output:
                while remaining:
                    if time.monotonic() - started > 300:
                        raise ValueError('Anonymous bottle acquisition deadline exceeded')
                    data = response.read(min(1 << 20, remaining))
                    if not data:
                        raise ValueError('Anonymous bottle was truncated')
                    output.write(data)
                    remaining -= len(data)
                if response.read(1):
                    raise ValueError('Anonymous bottle exceeds its locked size')
            if digest(part) != asset['sha256']:
                raise ValueError('Anonymous bottle differs from its immutable digest')
            part.rename(target)
        finally:
            part.unlink(missing_ok=True)
    if target.is_symlink() or not target.is_file() or target.stat().st_size != asset['size_bytes'] or digest(target) != asset['sha256']:
        raise ValueError('Cached bottle differs from its immutable public identity')
    return target


def run(arguments, environment, cwd=None, timeout=120):
    result = subprocess.run([str(value) for value in arguments], cwd=cwd, env=environment,
                            check=True, capture_output=True, text=True, timeout=timeout)
    if len(result.stdout) > 1 << 20 or len(result.stderr) > 1 << 20:
        raise ValueError('Native tool observation exceeds its bound')
    return result.stdout.strip()


def target_path(reference, cellar, versions, origin):
    for prefix in ('@@HOMEBREW_CELLAR@@/', '/opt/homebrew/Cellar/'):
        if reference.startswith(prefix):
            path = cellar / reference.removeprefix(prefix)
            break
    else:
        for prefix in ('@@HOMEBREW_PREFIX@@/opt/', '/opt/homebrew/opt/'):
            if reference.startswith(prefix):
                package, separator, rest = reference.removeprefix(prefix).partition('/')
                if not separator or package not in versions:
                    raise ValueError('Mach-O dependency is outside the declared bottle closure')
                path = cellar / package / versions[package] / rest
                break
        else:
            if reference.startswith('@loader_path/'):
                path = origin.parent / reference.removeprefix('@loader_path/')
            else:
                raise ValueError('Mach-O dependency requires an unqualified host fallback')
    path = path.resolve(strict=True)
    if not path.is_relative_to(cellar.resolve()) or not path.is_file():
        raise ValueError('Mach-O reference escapes the private admitted Cellar')
    return path


def validate_macho(path):
    slices = macho_metadata.inspect(path)
    if len(slices) != 1 or slices[0]['architecture'] != 'arm64' or slices[0]['minimum_os'] > [15, 0, 0]:
        raise ValueError('Native bottle lacks its exact arm64/macOS15 deployment contract')
    return slices[0]


def relocate(recipe, root, environment):
    cellar = root / 'gio/Cellar'
    versions = {entry['package']: entry['version'] for entry in recipe['archives'] if entry['package'] in PACKAGES}
    admitted = {(root / record['path']).resolve() for record in recipe['libraries']}
    transformations = []
    for record in recipe['libraries']:
        binary = root / record['path']
        before = digest(binary)
        metadata = validate_macho(binary)
        for reference in metadata['dependencies']:
            if reference.startswith(('/usr/lib/', '/System/Library/')):
                continue
            selected = target_path(reference, cellar, versions, binary)
            if selected not in admitted:
                raise ValueError('Bottle runtime closure requires an undeclared library')
            run(['/usr/bin/install_name_tool', '-change', reference, selected, binary], environment)
        run(['/usr/bin/install_name_tool', '-id', binary, binary], environment)
        run(['/usr/bin/codesign', '--force', '--sign', '-', binary], environment)
        transformations.append({'path': record['path'], 'source_sha256': before, 'derived_sha256': digest(binary)})
    for path in sorted(cellar.rglob('*.pc')):
        if path.is_symlink() or not path.is_file() or path.stat().st_size > 65536:
            raise ValueError('pkg-config metadata must be bounded regular files')
        content = path.read_text()
        for package, version in versions.items():
            for prefix in ('@@HOMEBREW_PREFIX@@/opt/', '/opt/homebrew/opt/'):
                content = content.replace(prefix + package, str(cellar / package / version))
        content = content.replace('@@HOMEBREW_CELLAR@@', str(cellar)).replace('/opt/homebrew/Cellar', str(cellar))
        if '@@HOMEBREW' in content or '/opt/homebrew' in content:
            raise ValueError('pkg-config metadata retains an implicit host prefix')
        path.write_text(content)
    return transformations


def project_glib_licenses(recipe, archive_path, root):
    selected = {Path(record['path']).name.removeprefix('COPYING-source-'): record
                for record in recipe['licenses'] if record['archive_id'] == 'glib-source'}
    if not selected or len(selected) > 128:
        raise ValueError('GLib source license projection requires a bounded exact inventory')
    found = set()
    expanded = 0
    with tarfile.open(archive_path, 'r|xz') as archive:
        for count, member in enumerate(archive, 1):
            expanded += max(member.size, 0)
            if count > 60000 or member.size < 0 or member.size > MAX_ASSET or expanded > 2 << 30:
                raise ValueError('GLib source metadata exceeds its projection bound')
            prefix = 'glib-2.90.1/LICENSES/'
            if not member.name.startswith(prefix):
                continue
            name = member.name.removeprefix(prefix)
            if name not in selected:
                continue
            record = selected[name]
            if name in found or not member.isfile() or member.size != record['size_bytes'] or member.size > 1 << 20:
                raise ValueError('GLib source license member differs from its pinned inventory')
            found.add(name)
            destination = root / record['path']
            with archive.extractfile(member) as stream, destination.open('xb') as output:
                shutil.copyfileobj(stream, output, 65536)
            if digest(destination) != record['sha256']:
                raise ValueError('GLib projected license digest mismatch')
    if found != set(selected):
        raise ValueError('GLib source omitted a required license member')


def prepare(recipe, root, toolchain, cache, fetch, acquire, environment):
    validate(recipe)
    if platform.system() != 'Darwin' or platform.machine() != 'arm64' or platform.mac_ver()[0].split('.')[0] != '15':
        raise ValueError('This recipe builds only on the macos-15 native arm64 runner; newer native hosts are separate consumer proofs')
    if root.exists():
        raise ValueError('Use a fresh native Mac input workspace')
    root.mkdir(parents=True, mode=0o700)
    environment = dict(environment, CC='/usr/bin/clang', CXX='/usr/bin/clang++', MACOSX_DEPLOYMENT_TARGET='15.0')
    cmake = toolchain / 'bin/cmake'
    pkgconfig = toolchain / 'bin/pkg-config'
    if any(not path.is_absolute() or not path.is_file() for path in (cmake, pkgconfig)):
        raise ValueError('Select the explicit admitted native CMake/pkg-config toolchain')
    sdk = run(['/usr/bin/xcrun', '--sdk', 'macosx', '--show-sdk-path'], environment)
    if run(['/usr/bin/xcrun', '--sdk', 'macosx', '--show-sdk-version'], environment) != '15.5':
        raise ValueError('Native SDK must match the published macOS15/AppleClang17 baseline')
    sdk = str(Path(sdk).resolve(strict=True))
    environment['SDKROOT'] = sdk
    probe = root / 'compiler-probe'
    probe.mkdir()
    (probe / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.21)\nproject(MacRecipeProbe LANGUAGES CXX)\nfile(WRITE "${CMAKE_BINARY_DIR}/identity.json" "{\\"id\\":\\"${CMAKE_CXX_COMPILER_ID}\\",\\"version\\":\\"${CMAKE_CXX_COMPILER_VERSION}\\"}")\n')
    run([cmake, '-S', probe, '-B', probe / 'build', '-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0'], environment)
    identity = json.loads((probe / 'build/identity.json').read_text())
    if identity != {key: BASELINE[key] for key in ('id', 'version')}:
        raise ValueError('Actual CMake compiler differs from the pinned native SDK/host ABI')
    paths = {}
    for entry in recipe['archives']:
        if entry['package'] in PACKAGES and entry['id'] != 'glib-source':
            path = acquire_oci(entry, cache, fetch)
        else:
            path = acquire(entry['asset'], cache, fetch, upstream=True)
        asset = entry['asset']
        if path.is_symlink() or not path.is_file() or path.stat().st_size != asset['size_bytes'] or digest(path) != asset['sha256']:
            raise ValueError('The acquired native input differs from its declared public source')
        paths[entry['id']] = path
    def locked(entry):
        return {'path': paths[entry['id']], 'sha256': entry['asset']['sha256'], 'size_bytes': entry['asset']['size_bytes']}
    qt_records = [locked(entry) for entry in recipe['archives'] if entry['id'] in {'qtbase', 'qttools'}]
    qt = root / 'qt/6.10.3/macos'
    framework_extract.extract_locked(qt_records, qt)
    bottle_records = [locked(entry) for entry in recipe['archives'] if entry['id'] in PACKAGES]
    framework_extract.extract_locked(bottle_records, root / 'gio/Cellar')
    source_entry = next(entry for entry in recipe['archives'] if entry['id'] == 'dbus-source')
    framework_extract.extract_locked([locked(source_entry)], root / 'broker-unpack')
    (root / 'broker-unpack/dbus-1.16.2').rename(root / 'broker-source')
    project_glib_licenses(recipe, paths['glib-source'], root)
    for record in recipe['licenses'] + recipe['libraries']:
        path = root / record['path']
        if path.is_symlink() or not path.is_file() or path.stat().st_size != record['size_bytes'] or digest(path) != record['sha256']:
            raise ValueError('Mac dependency source inventory differs from its public pins')
    transformations = relocate(recipe, root, environment)
    (qt / 'bin/qt.conf').write_text('[Paths]\nPrefix=..\n')
    if run([qt / 'bin/qmake', '-query', 'QT_VERSION'], environment) != '6.10.3':
        raise ValueError('The extracted native Qt kit version differs')
    gio = root / 'gio/Cellar/glib/2.90.1'
    environment.update(DOCKPIPE_VM_DISPLAY_BREW_PREFIX=str(root / 'gio'),
                       PKG_CONFIG=str(pkgconfig), PKG_CONFIG_PATH='', PKG_CONFIG_SYSROOT_DIR='',
                       PKG_CONFIG_LIBDIR=os.pathsep.join(str(root / 'gio/Cellar' / entry['package'] / entry['version'] / 'lib/pkgconfig')
                           for entry in recipe['archives'] if entry['id'] in PACKAGES))
    pkgconfig_metadata = private_pkgconfig.admit_gio_graph(pkgconfig, environment, root / 'gio/Cellar')
    environment['PATH'] = os.pathsep.join((str(toolchain / 'bin'), str(qt / 'bin'), str(gio / 'bin'), '/usr/bin', '/bin', '/usr/sbin', '/sbin'))
    source = root / 'broker-source'
    patch = Path(__file__).with_name('macos-dbus-peerpid.patch')
    if digest(patch) != PATCH_SHA256:
        raise ValueError('The private broker credential patch differs from its qualified source')
    run(['/usr/bin/patch', '-p1', '-F', '0', '-i', patch], environment, cwd=source)
    build = root / 'broker-build'
    run([cmake, '-S', source, '-B', build, '-G', 'Unix Makefiles', '-DCMAKE_BUILD_TYPE=Release',
         '-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0', '-DCMAKE_OSX_SYSROOT=' + sdk,
         '-DDBUS_BUILD_TESTS=OFF', '-DDBUS_BUILD_X11=OFF', '-DDBUS_ENABLE_MODULAR_TESTS=OFF',
         '-DEXPAT_INCLUDE_DIR=' + sdk + '/usr/include', '-DEXPAT_LIBRARY=' + sdk + '/usr/lib/libexpat.tbd'], environment)
    run([cmake, '--build', build, '--target', 'dbus-daemon', '--parallel', '2'], environment, timeout=300)
    broker = build / 'bin/dbus-daemon'
    runtime = build / 'lib/libdbus-1.3.dylib'
    if not runtime.resolve(strict=True).is_relative_to(build.resolve()):
        raise ValueError('Private broker runtime escaped its selected source build')
    validate_macho(broker)
    validate_macho(runtime.resolve(strict=True))
    licenses = [{'path': str(source / name), 'sha256': digest(source / name)}
                for name in ('COPYING', 'LICENSES/AFL-2.1.txt', 'LICENSES/GPL-2.0-or-later.txt')]
    provenance = root / 'broker-provenance.json'
    provenance.write_text(json.dumps({'schema': 1, 'version': '1.16.2', 'binary_sha256': digest(broker),
        'source_url': source_entry['asset']['url'], 'source_sha256': source_entry['asset']['sha256'],
        'patch_sha256': PATCH_SHA256, 'runtime_libraries': [{'path': str(runtime), 'sha256': digest(runtime)}],
        'licenses': licenses}, indent=2) + '\n')
    (root / 'macos-input-derivations.json').write_text(json.dumps({'schema': 1, 'source_libraries': transformations,
        'deployment_target': '15.0', 'sdk_version': '15.5', 'compiler': identity,
        'private_pkgconfig': pkgconfig_metadata, 'native_store_qualified': False}, indent=2) + '\n')
    return {'qt': qt, 'gio': gio, 'broker': broker.parent, 'broker_provenance': provenance}, environment
