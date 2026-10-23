"""Bounded official multiarchive inputs; no package hooks or implicit host fallback."""
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import platform
import posixpath
import re
import shutil
import subprocess
import tarfile
import tempfile
import urllib.parse

DERIVED_ROLES = {'qt', 'gio', 'broker', 'broker_provenance'}
PREFIXES = {'download.qt.io': '/online/qtsdkrepository/',
            'repo.msys2.org': '/mingw/mingw64/',
            'archive.ubuntu.com': '/ubuntu/pool/',
            'dbus.freedesktop.org': '/releases/dbus/'}
MAX_FILES = 50000
MAX_EXPANDED = 4 << 30
MAX_FILE = 256 << 20


def unique_fields(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate native preparation metadata field')
        result[key] = value
    return result


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            result.update(block)
    return result.hexdigest()


def relative(name, empty=False):
    if empty and name == '':
        return name
    if (not isinstance(name, str) or not name or len(name.encode('utf-8')) > 1024 or PurePosixPath(name).is_absolute()
            or PurePosixPath(name).as_posix() != name
            or any(part in {'.', '..'} for part in PurePosixPath(name).parts)
            or any(character in name for character in '\\:\x00\r\n')):
        raise ValueError('Uncontained native input path')
    return name


def validate_asset(asset):
    required = {'url', 'sha256', 'size_bytes', 'format', 'root'}
    if not isinstance(asset, dict) or not required.issubset(asset) or set(asset) - required - {'redirect_hosts'}:
        raise ValueError('Official archive requires exact immutable input fields')
    if not isinstance(asset['url'], str):
        raise ValueError('Official input URL must be a string')
    url = urllib.parse.urlsplit(asset['url'])
    if (url.scheme != 'https' or url.netloc not in PREFIXES or url.query or url.fragment
            or not url.path.startswith(PREFIXES[url.netloc])
            or '..' in urllib.parse.unquote(url.path).split('/')):
        raise ValueError('Native archive must use its exact official upstream URL')
    if (not isinstance(asset['sha256'], str) or not re.fullmatch('[0-9a-f]{64}', asset['sha256'])
            or type(asset['size_bytes']) is not int or not 0 < asset['size_bytes'] <= MAX_FILE
            or not isinstance(asset['format'], str) or asset['format'] not in {'7z', 'deb', 'pkg.tar.zst', 'tar.xz'}):
        raise ValueError('Native archive identity or capacity is invalid')
    relative(asset['root'], empty=True)
    hosts = asset.get('redirect_hosts', [])
    if (not isinstance(hosts, list) or len(hosts) > 64 or any(not isinstance(host, str) for host in hosts) or len(set(hosts)) != len(hosts)
            or any(not isinstance(host, str) or not re.fullmatch(r'[a-z0-9-]+(?:\.[a-z0-9-]+)+', host)
                   or host.endswith(('.local', '.localhost', '.internal')) or re.fullmatch(r'[0-9.]+', host) for host in hosts)):
        raise ValueError('Native redirect hosts must be exact reviewed public Metalink hostnames')
    if hosts and url.hostname != 'download.qt.io':
        raise ValueError('Only official Qt Metalink mirrors may extend native input redirects')


def validate(recipe, target):
    if (not isinstance(recipe, dict) or set(recipe) != {'schema', 'platform', 'compiler', 'archives', 'licenses', 'libraries'}
            or type(recipe['schema']) is not int or recipe['schema'] != 1 or recipe['platform'] != target):
        raise ValueError('Unsupported native multiarchive recipe')
    expected = ({'id': 'GNU', 'version': '11.4.0', 'os_id': 'ubuntu', 'os_version': '22.04'}
                if target == 'linux-amd64' else {'id': 'MSVC', 'version': '19.44.35229.0', 'os_id': 'windows', 'os_version': 'native'})
    compiler = dict(recipe['compiler']) if isinstance(recipe['compiler'], dict) else {}
    if target == 'windows-amd64':
        acquisition = compiler.pop('acquisition_sha256', '')
        if not isinstance(acquisition, str) or not re.fullmatch('[0-9a-f]{64}', acquisition):
            raise ValueError('MSVC requires source-bound acquisition metadata')
    if target not in {'linux-amd64', 'windows-amd64'} or compiler != expected:
        raise ValueError('Native recipe requires its exact qualified compiler and OS baseline')
    entries = recipe['archives']
    if not isinstance(entries, list) or not 0 < len(entries) <= 64:
        raise ValueError('Native recipe archive count exceeds its bound')
    seen = set()
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != {'id', 'destination', 'asset', 'package', 'version'}:
            raise ValueError('Native archive record is incomplete')
        name = entry['id']
        if not isinstance(name, str) or not re.fullmatch('[A-Za-z0-9_+.-]{1,128}', name) or name in seen or name in {'.', '..'}:
            raise ValueError('Duplicate or unsafe native archive identity')
        seen.add(name)
        relative(entry['destination'])
        if entry['destination'].split('/')[0] not in {'qt', 'gio', 'broker-source'}:
            raise ValueError('Native archive has an undeclared output role')
        validate_asset(entry['asset'])
        if (not isinstance(entry['package'], str) or not isinstance(entry['version'], str)
                or not 0 < len(entry['package']) <= 128 or not 0 < len(entry['version']) <= 128):
            raise ValueError('Native package identity is incomplete')
    if sum(entry['asset']['size_bytes'] for entry in entries) > 512 << 20:
        raise ValueError('Native recipe acquisition exceeds its capacity')
    for field, maximum in (('licenses', 256), ('libraries', 64)):
        records = recipe[field]
        if not isinstance(records, list) or not records or len(records) > maximum:
            raise ValueError('Native license/library inventory is incomplete')
        paths = set()
        for record in records:
            if not isinstance(record, dict) or set(record) != {'path', 'sha256', 'size_bytes', 'package', 'version', 'archive_id'}:
                raise ValueError('Native file provenance is incomplete')
            relative(record['path'])
            if (record['path'].casefold() in paths or not isinstance(record['archive_id'], str) or record['archive_id'] not in seen
                    or not isinstance(record['sha256'], str) or not re.fullmatch('[0-9a-f]{64}', record['sha256'])
                    or type(record['size_bytes']) is not int or not 0 < record['size_bytes'] <= MAX_FILE):
                raise ValueError('Native file provenance differs from declared packages')
            owner = next(entry for entry in entries if entry['id'] == record['archive_id'])
            if record['package'] != owner['package'] or record['version'] != owner['version']:
                raise ValueError('Native license/library owner differs from exact package identity')
            paths.add(record['path'].casefold())
    return recipe


class BoundedTarInfo(tarfile.TarInfo):
    def _proc_member(self, archive):
        if self.type in {tarfile.XHDTYPE, tarfile.XGLTYPE, tarfile.GNUTYPE_LONGNAME, tarfile.GNUTYPE_LONGLINK} and self.size > 65536:
            raise ValueError('Native tar metadata exceeds its bound')
        if self.type == tarfile.GNUTYPE_SPARSE:
            raise ValueError('Sparse native input is unsupported')
        return super()._proc_member(archive)


class PrefixWriter:
    def __init__(self, root, case_sensitive=False):
        if case_sensitive and os.name == 'nt':
            raise ValueError('Linux archive paths require a native case-sensitive host')
        self.case_sensitive = case_sensitive
        self.root = root
        self.names = {}
        self.aliases = {}
        self.count = 0
        self.expanded = 0

    def key(self, name):
        return name if self.case_sensitive else name.casefold()

    def reserve(self, name, size):
        relative(name)
        self.count += 1
        self.expanded += size
        if (self.count > MAX_FILES or self.expanded > MAX_EXPANDED
                or not 0 <= size <= MAX_FILE):
            raise ValueError('Native archive expansion exceeds its bounds')
        folded = self.key(name)
        if folded in self.names and self.names[folded] != name:
            raise ValueError('Native input has a case-colliding path')
        self.names[folded] = name

    def write(self, name, size, mode, incoming):
        self.reserve(name, size)
        target = self.root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryFile() as temporary:
            checksum = hashlib.sha256()
            remaining = size
            while remaining:
                block = incoming.read(min(1 << 20, remaining))
                if not block:
                    raise ValueError('Truncated native archive member')
                temporary.write(block)
                checksum.update(block)
                remaining -= len(block)
            if target.exists():
                if target.is_symlink() or not target.is_file() or target.stat().st_size != size or digest(target) != checksum.hexdigest():
                    raise ValueError('Native archives overlap with different bytes')
                return
            temporary.seek(0)
            with target.open('xb') as output:
                shutil.copyfileobj(temporary, output, 1 << 20)
        target.chmod(0o755 if mode & 0o111 else 0o644)

    def link(self, name, target, base, hard=False):
        self.reserve(name, len(target.encode()))
        if '\\' in target or '\x00' in target or len(target.encode()) > 4096:
            raise ValueError('Invalid native archive alias')
        path = target.lstrip('/') if target.startswith('/') or hard else posixpath.join(posixpath.dirname(name.removeprefix(base + '/')), target)
        resolved = posixpath.normpath(path)
        relative(resolved)
        if self.case_sensitive and base == 'gio' and resolved.split('/')[0] in {'lib', 'bin', 'sbin'}:
            resolved = 'usr/' + resolved
        resolved = base + '/' + resolved
        if name in self.aliases and self.aliases[name] != resolved:
            raise ValueError('Conflicting native archive alias')
        self.aliases[name] = resolved

    def finish_aliases(self):
        pending = dict(self.aliases)
        for _ in range(8):
            progress = False
            for name, value in list(pending.items()):
                source = self.root / value
                if not source.exists():
                    continue
                files = [(source, name)] if source.is_file() else [(path, name + '/' + path.relative_to(source).as_posix()) for path in source.rglob('*') if path.is_file()]
                for path, destination in files:
                    if path.is_symlink():
                        raise ValueError('Native prefix contains a host filesystem link')
                    with path.open('rb') as incoming:
                        self.write(destination, path.stat().st_size, path.stat().st_mode, incoming)
                del pending[name]
                progress = True
            if not pending:
                return
            if not progress:
                break
        raise ValueError('Native aliases escape, cycle or refer to missing material: ' + ', '.join(sorted(pending)[:8]))


def member_name(entry, name):
    name = name.removeprefix('./')
    if name in {'', '.'}:
        return None
    relative(name.rstrip('/'))
    root = entry['asset']['root']
    if root:
        if name.rstrip('/') == root:
            return None
        if not name.startswith(root + '/'):
            if entry['asset']['format'] == 'pkg.tar.zst' and (name in {'.PKGINFO', '.BUILDINFO', '.MTREE', '.INSTALL'} or name.startswith('usr/share/libalpm/hooks/') or name.rstrip('/') in {'usr', 'usr/share', 'usr/share/libalpm', 'usr/share/libalpm/hooks'}):
                return None
            raise ValueError('Native archive member is outside its locked root')
        name = name[len(root) + 1:]
    if entry['asset']['format'] == 'deb' and name.split('/')[0] in {'lib', 'bin', 'sbin'}:
        name = 'usr/' + name
    return entry['destination'] + '/' + name.rstrip('/')


def tar_members(entry, stream, writer):
    with tarfile.open(fileobj=stream, mode='r|*', tarinfo=BoundedTarInfo) as archive:
        seen = {}
        for item in archive:
            if not 0 <= item.size <= MAX_FILE:
                raise ValueError('Native tar member exceeds its declared bound')
            name = member_name(entry, item.name)
            if name is None:
                writer.count += 1
                writer.expanded += item.size
                if writer.count > MAX_FILES or writer.expanded > MAX_EXPANDED:
                    raise ValueError('Ignored archive metadata exceeds native capacity')
                continue
            if writer.key(name) in seen:
                if item.isdir() and seen[writer.key(name)]:
                    writer.reserve(name, 0)
                    continue
                raise ValueError('Duplicate member inside native archive: ' + name)
            seen[writer.key(name)] = item.isdir()
            if item.isdir():
                writer.reserve(name, 0)
            elif item.issym() or item.islnk():
                target = item.linkname
                archive_root = entry['asset']['root']
                if item.islnk() and archive_root and target.startswith(archive_root + '/'):
                    target = target[len(archive_root) + 1:]
                writer.link(name, target, entry['destination'], hard=item.islnk())
            elif item.isfile() and not item.sparse:
                with archive.extractfile(item) as incoming:
                    writer.write(name, item.size, item.mode, incoming)
            else:
                raise ValueError('Native archive contains a device or sparse member')


def deb_members(entry, path, writer):
    with path.open('rb') as source:
        if source.read(8) != b'!<arch>\n':
            raise ValueError('Invalid Debian archive header')
        found = False
        while header := source.read(60):
            if len(header) != 60 or header[58:] != b'`\n':
                raise ValueError('Invalid Debian member header')
            size = int(header[48:58])
            name = header[:16].decode('ascii').strip().rstrip('/')
            if not 0 <= size <= MAX_FILE:
                raise ValueError('Debian member exceeds capacity')
            payload = source.read(size)
            if len(payload) != size:
                raise ValueError('Truncated Debian member')
            if size % 2:
                source.read(1)
            if name.startswith('data.tar.'):
                if found or name not in {'data.tar.xz', 'data.tar.gz', 'data.tar.zst'}:
                    raise ValueError('Duplicate or unsupported Debian data stream')
                found = True
                if name == 'data.tar.zst':
                    import pyzstd
                    if pyzstd.__version__ != '0.20.0':
                        raise ValueError('Use the locked native pyzstd0.20.0 extractor')
                    with pyzstd.ZstdFile(io.BytesIO(payload), 'rb') as stream:
                        tar_members(entry, stream, writer)
                else:
                    tar_members(entry, io.BytesIO(payload), writer)
        if not found:
            raise ValueError('Debian archive has no data stream')


def sevenzip_members(entry, path, writer):
    import py7zr
    if py7zr.__version__ != '1.0.0':
        raise ValueError('Use the locked py7zr1.0.0 extractor environment')
    products = []
    with py7zr.SevenZipFile(path) as archive:
        records = {}
        if len(archive.files) > MAX_FILES or sum(item.uncompressed for item in archive.files) > MAX_EXPANDED:
            raise ValueError('Qt archive expansion exceeds its declared capacity')
        for item in archive.files:
            name = member_name(entry, item.filename)
            if name is None:
                continue
            if name in records or item.is_junction or item.is_socket:
                raise ValueError('Duplicate or unsupported Qt archive member')
            records[name] = item
            if item.is_directory:
                writer.reserve(name, 0)
            elif not 0 <= item.uncompressed <= MAX_FILE:
                raise ValueError('Qt archive member exceeds capacity')
        with tempfile.TemporaryDirectory(prefix=".native-7z-", dir=writer.root) as temporary:
            class Product:
                def __init__(self, name, item):
                    self.name, self.item = name, item
                    self.path = Path(temporary) / str(len(products))
                    self.path.touch()
                    self.position = 0
                    products.append(self)

                def write(self, data):
                    if self.position + len(data) > self.item.uncompressed:
                        raise ValueError('Qt member expanded beyond its declaration')
                    with self.path.open('r+b') as stream:
                        stream.seek(self.position)
                        count = stream.write(data)
                    self.position += count
                    return count

                def read(self, size=None):
                    with self.path.open('rb') as stream:
                        stream.seek(self.position)
                        data = stream.read(-1 if size is None else size)
                    self.position += len(data)
                    return data

                def seek(self, offset, whence=0):
                    if whence == 0:
                        position = offset
                    elif whence == 1:
                        position = self.position + offset
                    else:
                        position = self.path.stat().st_size + offset
                    if not 0 <= position <= self.item.uncompressed:
                        raise ValueError('Qt extractor requested an invalid bounded seek')
                    self.position = position
                    return position

                def flush(self):
                    return None

                def size(self):
                    return self.path.stat().st_size

                def close(self):
                    # py7zr1.0 does not call this hook. No open descriptor is
                    # retained; completion is admitted after extraction returns.
                    return None

                def finish(self):
                    if self.path.stat().st_size != self.item.uncompressed:
                        raise ValueError('Qt archive member did not finish')
                    with self.path.open('rb') as incoming:
                        if self.item.is_symlink:
                            writer.link(self.name, incoming.read(4097).decode('utf-8'), entry['destination'])
                        else:
                            writer.write(self.name, self.item.uncompressed, self.item.posix_mode or 0o644, incoming)

            class Factory:
                def create(self, filename):
                    raw = Path(filename).relative_to(writer.root.resolve()).as_posix()
                    return Product(raw, records[raw])

            archive.extractall(path=(writer.root / entry['destination']).resolve(), factory=Factory())
            for product in products:
                product.finish()


def unpack(entry, archive, writer, toolchain):
    asset = entry['asset']
    if archive.is_symlink() or archive.stat().st_size != asset['size_bytes'] or digest(archive) != asset['sha256']:
        raise ValueError('Native input changed after acquisition')
    if asset['format'] == '7z':
        sevenzip_members(entry, archive, writer)
    elif asset['format'] == 'deb':
        deb_members(entry, archive, writer)
    elif asset['format'] == 'tar.xz':
        with archive.open('rb') as stream:
            tar_members(entry, stream, writer)
    else:
        import pyzstd
        if pyzstd.__version__ != '0.20.0':
            raise ValueError('Use the locked native pyzstd0.20.0 extractor')
        with pyzstd.ZstdFile(archive, 'rb') as stream:
            tar_members(entry, stream, writer)


def verify_files(recipe, root):
    for record in recipe['licenses'] + recipe['libraries']:
        path = root / record['path']
        if (path.is_symlink() or not path.is_file() or path.stat().st_size != record['size_bytes']
                or digest(path) != record['sha256']):
            raise ValueError('Extracted native license/library differs from its public package inventory')


def compiler_environment(recipe, toolchain, environment):
    expected = recipe['compiler']
    result = {key: value for key, value in environment.items()
              if key not in {'GH_TOKEN', 'GITHUB_TOKEN', 'GITHUB_ENTERPRISE_TOKEN', 'GH_ENTERPRISE_TOKEN'}}
    system_path = (str(Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32')
                   if os.name == 'nt' else os.defpath)
    result['PATH'] = str(toolchain / 'bin') + os.pathsep + system_path
    if expected['id'] == 'GNU':
        os_release = platform.freedesktop_os_release()
        if os_release.get('ID') != 'ubuntu' or os_release.get('VERSION_ID') != '22.04':
            raise ValueError('GNU11.4 recipe requires the qualified Ubuntu22.04 native baseline')
        compiler = toolchain / 'bin/g++'
        actual = subprocess.check_output([str(compiler), '-dumpfullversion'], text=True, timeout=15).strip()
        if actual != expected['version']:
            raise ValueError('Native recipe compiler differs from qualified GNU11.4')
        result.update(CC=str(toolchain / 'bin/gcc'), CXX=str(compiler))
    else:
        selected_root = result.get('DOCKPIPE_NATIVE_MSVC_ROOT', '')
        receipt_path = result.get('DOCKPIPE_NATIVE_MSVC_RECEIPT', '')
        if not selected_root or not receipt_path or not Path(selected_root).is_absolute() or not Path(receipt_path).is_absolute():
            raise ValueError('Run the source-bound hosted MSVC preparation; no redistributed compiler fallback')
        installation = Path(selected_root)
        path = Path(receipt_path)
        if path.is_symlink() or not path.is_file() or path.stat().st_size > 65536:
            raise ValueError('Native MSVC receipt is missing or unbounded')
        receipt = json.loads(path.read_bytes(), object_pairs_hook=unique_fields)
        if (not isinstance(receipt, dict) or set(receipt) != {'schema', 'compiler_id', 'compiler_version', 'windows_sdk_version',
                             'acquisition_sha256', 'channel_sha256', 'bootstrapper_sha256', 'tools'}
                or type(receipt['schema']) is not int or receipt['schema'] != 1
                or receipt['compiler_id'] != 'MSVC' or receipt['compiler_version'] != expected['version']
                or receipt['windows_sdk_version'] != '10.0.26100.0'
                or receipt['acquisition_sha256'] != expected['acquisition_sha256']
                or receipt['channel_sha256'] != '4f0f290bb4d58a061a49a2fa893357a8c9e7e2b49fc91873f533ab8f791e4fa0'
                or receipt['bootstrapper_sha256'] != '68978bb28916156802e60e04c5ee50b94348d802fe151a3ca24433f27984187b'
                or not isinstance(receipt['tools'], dict) or set(receipt['tools']) != {'cl', 'dumpbin', 'lib'}):
            raise ValueError('Native MSVC preparation differs from its source-bound ABI/channel')
        tools = {}
        for name, record in receipt['tools'].items():
            if not isinstance(record, dict) or set(record) != {'path', 'sha256'}:
                raise ValueError('Invalid native compiler tool receipt')
            tool = installation / relative(record['path'])
            if (any(parent.is_symlink() for parent in (tool, *tool.parents)) or not tool.is_file()
                    or tool.stat().st_size > 128 << 20 or digest(tool) != record['sha256']):
                raise ValueError('Native compiler tool differs from prepared signed installation')
            tools[name] = tool
        compiler = tools['cl']
        result['PATH'] = str(compiler.parent) + os.pathsep + result['PATH']
        result_process = subprocess.run([str(compiler), '/Bv'], capture_output=True, text=True, timeout=15)
        if not re.search(r'Compiler Version ' + re.escape(expected['version'].removesuffix('.0')) + r'\b', result_process.stdout + result_process.stderr):
            raise ValueError('Native recipe compiler differs from qualified MSVC19.44')
        if not result.get('INCLUDE') or not result.get('LIB'):
            raise ValueError('Explicit approved MSVC environment is required; no compiler installer fallback')
        result.update(CC=str(compiler), CXX=str(compiler),
                      DOCKPIPE_NATIVE_DUMPBIN=str(tools['dumpbin']), DOCKPIPE_NATIVE_LIB=str(tools['lib']),
                      DOCKPIPE_VM_DISPLAY_INSPECTOR=str(tools['dumpbin']))
    return result


def derive_imports(root, toolchain, environment):
    gio = root / 'gio'
    for name in ('gio', 'gobject', 'glib'):
        binary = gio / 'bin' / ('lib' + name + '-2.0-0.dll')
        output = subprocess.check_output([environment['DOCKPIPE_NATIVE_DUMPBIN'], '/nologo', '/exports', str(binary)],
                                         text=True, env=environment, timeout=30)
        if len(output) > 4 << 20:
            raise ValueError('MSVC export metadata exceeds its bound')
        symbols = re.findall(r'^\s+\d+\s+[A-Fa-f0-9]+\s+[A-Fa-f0-9]+\s+([A-Za-z_][A-Za-z0-9_@$?]*)\s*$', output, re.MULTILINE)
        if not 10 <= len(symbols) <= 30000 or len(set(symbols)) != len(symbols):
            raise ValueError('Invalid pinned GIO DLL export inventory')
        definition = gio / 'lib' / (name + '-2.0.def')
        definition.write_text('LIBRARY ' + binary.name + '\nEXPORTS\n' + '\n'.join(symbols) + '\n', encoding='ascii')
        subprocess.run([environment['DOCKPIPE_NATIVE_LIB'], '/nologo', '/machine:x64', '/def:' + str(definition),
                        '/out:' + str(gio / 'lib' / (name + '-2.0.lib'))], env=environment, check=True, timeout=30)



def write_linux_receipt(recipe, root):
    gio = root / 'gio/usr'
    receipt = root / 'linux-prefix-receipt.json'
    receipt.write_text(json.dumps({'schema': 1, 'prefix': str(gio),
        'packages': [{'id': entry['id'], 'package': entry['package'], 'version': entry['version'],
                      'archive_sha256': entry['asset']['sha256']}
                     for entry in recipe['archives'] if entry['asset']['format'] == 'deb'],
        'licenses': [{**record, 'path': record['path'].removeprefix('gio/usr/')}
                     for record in recipe['licenses']],
        'libraries': [{**record, 'path': record['path'].removeprefix('gio/usr/')}
                      for record in recipe['libraries']]}, indent=2) + '\n', encoding='utf-8')
    return receipt


def prepare(recipe, root, toolchain, cache, fetch, acquire, environment):
    validate(recipe, recipe['platform'])
    environment = compiler_environment(recipe, toolchain, environment)
    root.mkdir(exist_ok=False)
    writer = PrefixWriter(root, case_sensitive=recipe['platform'] == 'linux-amd64')
    for entry in recipe['archives']:
        archive = acquire(entry['asset'], cache, fetch, upstream=True)
        unpack(entry, archive, writer, toolchain)
    writer.finish_aliases()
    verify_files(recipe, root)
    qt = root / ('qt/6.10.3/gcc_64' if recipe['platform'] == 'linux-amd64' else 'qt')
    (qt / 'bin/qt.conf').write_text('[Paths]\nPrefix=..\n', encoding='utf-8')
    environment['PATH'] = os.pathsep.join((str(qt / 'bin'), str(root / 'gio/bin'), environment['PATH']))
    actual_qt = subprocess.check_output([str(qt / 'bin' / ('qmake.exe' if os.name == 'nt' else 'qmake')), '-query', 'QT_VERSION'],
                                      env=environment, text=True, timeout=15).strip()
    if actual_qt != '6.10.3':
        raise ValueError('Extracted Qt differs from exact native SDK kit')
    if recipe['platform'] == 'windows-amd64':
        derive_imports(root, toolchain, environment)
        gio = root / 'gio'
        broker = gio / 'bin/dbus-daemon.exe'
        source = next(entry for entry in recipe['archives'] if entry['package'] == 'mingw-w64-x86_64-dbus')
        license_root = gio / 'share/licenses/dbus'
        runtime = []
    else:
        gio = root / 'gio/usr'
        environment.update(PKG_CONFIG_SYSROOT_DIR=str(root / 'gio'),
                           PKG_CONFIG_LIBDIR=os.pathsep.join(str(root / 'gio' / part) for part in
                               ('usr/lib/x86_64-linux-gnu/pkgconfig', 'usr/share/pkgconfig')),
                           LD_LIBRARY_PATH=str(gio / 'lib/x86_64-linux-gnu'))
        receipt = write_linux_receipt(recipe, root)
        environment.update(DOCKPIPE_VM_LINUX_DEPENDENCY_MODE='prefix', DOCKPIPE_VM_LINUX_PREFIX_RECEIPT=str(receipt))
        build = root / 'broker-build'
        arguments = [str(toolchain / 'bin/cmake'), '-S', str(root / 'broker-source'), '-B', str(build),
                     '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + str(toolchain / 'bin/ninja'), '-DCMAKE_BUILD_TYPE=Release', '-DDBUS_BUILD_TESTS=OFF',
                     '-DDBUS_BUILD_X11=OFF', '-DDBUS_RELOCATABLE=ON', '-DCMAKE_C_COMPILER=' + environment['CC'],
                     '-DCMAKE_CXX_COMPILER=' + environment['CXX'], '-DEXPAT_INCLUDE_DIR=' + str(gio / 'include'),
                     '-DEXPAT_LIBRARY=' + str(gio / 'lib/x86_64-linux-gnu/libexpat.so')]
        subprocess.run(arguments, env=environment, check=True, timeout=120)
        subprocess.run([str(toolchain / 'bin/cmake'), '--build', str(build), '--target', 'dbus-daemon', '--parallel', '2'],
                       env=environment, check=True, timeout=300)
        broker = build / 'bin/dbus-daemon'
        source = next(entry for entry in recipe['archives'] if entry['id'] == 'dbus-source')
        license_root = root / 'broker-source'
        libraries = sorted((build / 'lib').glob('libdbus-1.so.3.*'))
        if len(libraries) != 1:
            raise ValueError('Private broker has no unique built runtime library')
        runtime = [{'path': str(libraries[0]), 'sha256': digest(libraries[0])}]
    names = ['COPYING', 'AFL-2.1.txt', 'GPL-2.0-or-later.txt']
    licenses = []
    for name in names:
        path = license_root / name
        if not path.exists():
            path = license_root / 'LICENSES' / name
        if path.is_symlink() or not path.is_file() or path.stat().st_size > 1 << 20:
            raise ValueError('Private broker license text is missing or unbounded')
        licenses.append({'path': str(path), 'sha256': digest(path)})
    provenance = root / 'broker-provenance.json'
    provenance.write_text(json.dumps({'schema': 1, 'version': '1.16.2', 'binary_sha256': digest(broker),
        'source_url': source['asset']['url'], 'source_sha256': source['asset']['sha256'],
        'runtime_libraries': runtime, 'licenses': licenses}, indent=2) + '\n', encoding='utf-8')
    return {'qt': qt, 'gio': gio, 'broker': broker.parent, 'broker_provenance': provenance}, environment
