"""Draft VM-owned immutable native catalog producer. Never publishes or edits source inputs."""
import argparse
import catalog_transaction
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import tarfile
import time
import urllib.parse
import urllib.request
import zipfile

import native_inputs
import toolchain_inputs
import compatibility_overlay

MAX_INPUT = 2 * 1024 ** 3
MAX_FILES = 50000
MAX_EXPANDED = 4 * 1024 ** 3
SUPPORTED = {'linux-amd64', 'windows-amd64', 'darwin-arm64'}
ROLES = {'source', 'cli', 'display_sdk', 'qt', 'gio', 'toolchain', 'broker',
         'broker_provenance', 'go_sdk_archive', 'go_sdk_manifest', 'guest_tools', 'guest_descriptor', 'guest_release_manifest'}
REPOSITORY = 'Dockpipe-Industries/dockpipe-packages'


def sha256(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def read_json(path, maximum=1024 * 1024):
    if path.is_symlink() or not path.is_file() or path.stat().st_size > maximum:
        raise ValueError('Metadata must be a bounded regular file')
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('Duplicate metadata key')
            result[key] = value
        return result
    return json.loads(path.read_bytes(), object_pairs_hook=unique)


def relative(name):
    path = PurePosixPath(name)
    if (not name or path.is_absolute() or path.as_posix() != name
            or any(part in {'.', '..'} for part in path.parts)
            or any(character in name for character in '\\:\x00\r\n')):
        raise ValueError('Unsafe relative artifact path')
    return name


def validate_asset(asset):
    required = {'url', 'sha256', 'size_bytes', 'format', 'root'}
    if not isinstance(asset, dict) or set(asset) != required:
        raise ValueError('Asset requires exact public URL, hash, size, format and root')
    url = urllib.parse.urlsplit(asset['url'])
    if (url.scheme != 'https' or url.netloc != 'github.com' or url.query or url.fragment
            or not re.fullmatch(r'/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/(?:releases/download/[^/]+/[^/]+|archive/[0-9a-f]{40}\.tar\.gz)', url.path)
            or any(word in url.path.split('/') for word in ('latest', 'master', 'main'))):
        raise ValueError('Input must be an exact public release asset or full-commit source URL')
    if not isinstance(asset['sha256'], str) or not re.fullmatch('[0-9a-f]{64}', asset['sha256']):
        raise ValueError('Unresolved immutable input SHA-256')
    if type(asset['size_bytes']) is not int or not 0 < asset['size_bytes'] <= MAX_INPUT:
        raise ValueError('Unresolved or excessive immutable input size')
    if asset['format'] not in {'file', 'tar.gz', 'zip'}:
        raise ValueError('Unsupported acquisition format; a qualified producer is required')
    if asset['root']:
        relative(asset['root'])
    return asset


def load_lock(path, target, source_template=False):
    lock = read_json(path)
    if set(lock) != {'schema', 'source_commit', 'owned', 'profiles', 'accepted_platforms'} or lock['schema'] != 1:
        raise ValueError('Unsupported native store lock')
    accepted = lock.get('accepted_platforms')
    if (not isinstance(accepted, list) or not accepted or len(set(accepted)) != len(accepted)
            or any(item not in SUPPORTED for item in accepted)
            or set(lock.get('profiles', {})) != set(accepted)):
        raise ValueError('Exact explicitly accepted native platform set is required')
    if target not in accepted:
        raise ValueError('Platform has no admitted native catalog lane')
    if source_template:
        if lock['source_commit'] is not None:
            raise ValueError('Tracked dependency template must not contain a self-referential source commit')
    elif not isinstance(lock['source_commit'], str) or not re.fullmatch('[0-9a-f]{40}', lock['source_commit']):
        raise ValueError('An approved exact catalog source commit is required')
    owned = lock['owned']
    if (not isinstance(owned, list) or not owned or len(owned) > 16
            or any(not isinstance(item, list) or len(item) != 2 or item[0] not in {'workflow', 'resolver'}
                   or not re.fullmatch('[a-z0-9][a-z0-9._-]{0,127}', item[1]) for item in owned)
            or len({tuple(item) for item in owned}) != len(owned)):
        raise ValueError('Exact unique noncore package ownership is required')
    profile = lock['profiles'].get(target)
    if target == 'darwin-arm64' and (not isinstance(profile, dict) or 'native_inputs' not in profile):
        raise ValueError('Darwin requires its explicit native recipe and compiler/deployment baseline')
    required_profile = {'cli_version', 'inputs', 'previous_artifacts', 'guest_source_commit'}
    if not isinstance(profile, dict) or not required_profile.issubset(profile) or set(profile) - required_profile - {'native_inputs', 'toolchain_recipe', 'compatibility_overlay'}:
        raise ValueError('Native profile lacks a required public closure input')
    if 'compatibility_overlay' in profile:
        compatibility_overlay.validate(profile['compatibility_overlay'], validate_asset)
        if not all(item in owned for item in profile['compatibility_overlay']['owned']):
            raise ValueError('Native union ownership must preserve all compatibility workflows')
    expected_roles = ROLES
    if 'native_inputs' in profile:
        native_inputs.validate(profile['native_inputs'], target)
        expected_roles = ROLES - native_inputs.DERIVED_ROLES
    if 'toolchain_recipe' in profile:
        toolchain_inputs.validate_reference(profile['toolchain_recipe'])
        expected_roles = expected_roles - {'toolchain'}
    if not isinstance(profile['inputs'], dict) or set(profile['inputs']) != expected_roles:
        raise ValueError('Native profile mixes missing or duplicate direct/derived input roles')
    if not isinstance(profile['cli_version'], str) or not re.fullmatch(r'\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?', profile['cli_version']):
        raise ValueError('Exact immutable core CLI version is required')
    if not isinstance(profile['guest_source_commit'], str) or not re.fullmatch('[0-9a-f]{40}', profile['guest_source_commit']):
        raise ValueError('Exact independently pinned Guest Tools source commit is required')
    for role, asset in profile['inputs'].items():
        if role == 'source' and source_template:
            if asset != dict(url=None, sha256=None, size_bytes=None, format=None, root=None):
                raise ValueError('Tracked template source identity must be resolved by CI after the commit exists')
            continue
        validate_asset(asset)
    if not isinstance(profile['previous_artifacts'], dict):
        raise ValueError('Previous immutable archive identity map is required')
    if not source_template:
        expected_source = 'https://github.com/' + REPOSITORY + '/archive/' + lock['source_commit'] + '.tar.gz'
        if profile['inputs']['source']['url'] != expected_source:
            raise ValueError('Source archive URL must bind the exact approved catalog commit')
        if any(other['inputs']['source'] != profile['inputs']['source'] for other in lock['profiles'].values()):
            raise ValueError('Every platform must consume the same exact committed source archive')
    for role in ('cli', 'display_sdk', 'go_sdk_archive', 'go_sdk_manifest'):
        if not profile['inputs'][role]['url'].startswith('https://github.com/Dockpipe-Industries/dockpipe/releases/download/'):
            raise ValueError('Core-owned dependency must come from its owning public release')
    for name, digest in profile['previous_artifacts'].items():
        relative(name)
        if '/' in name or not re.fullmatch('[0-9a-f]{64}', digest):
            raise ValueError('Invalid prior immutable archive identity')
    return lock, profile


def acquire(asset, cache, fetch=False, *, upstream=False):
    if upstream:
        native_inputs.validate_asset(asset)
    else:
        validate_asset(asset)
    cache.mkdir(parents=True, exist_ok=True)
    destination = cache / asset['sha256']
    if not destination.exists():
        if not fetch:
            raise ValueError('Missing content-addressed public input; acquisition is explicit')
        temporary = destination.with_suffix('.part')
        started = time.monotonic()
        try:
            with urllib.request.urlopen(asset['url'], timeout=30) as response, temporary.open('xb') as output:
                final = urllib.parse.urlsplit(response.geturl())
                allowed_hosts = ({urllib.parse.urlsplit(asset['url']).hostname, *asset.get('redirect_hosts', [])} if upstream else
                                 {'github.com', 'release-assets.githubusercontent.com', 'objects.githubusercontent.com', 'codeload.github.com'})
                if final.scheme != 'https' or final.hostname not in allowed_hosts:
                    raise ValueError('Public asset redirected outside allowed hosts')
                remaining = asset['size_bytes']
                while remaining:
                    if time.monotonic() - started > 1200:
                        raise ValueError('Public asset acquisition deadline exceeded')
                    block = response.read(min(1024 * 1024, remaining))
                    if not block:
                        raise ValueError('Public asset was truncated')
                    output.write(block)
                    remaining -= len(block)
                if response.read(1):
                    raise ValueError('Public asset exceeded its locked size')
            if sha256(temporary) != asset['sha256']:
                raise ValueError('Public asset digest mismatch')
            temporary.rename(destination)
        finally:
            temporary.unlink(missing_ok=True)
    if destination.is_symlink() or not destination.is_file() or destination.stat().st_size != asset['size_bytes'] or sha256(destination) != asset['sha256']:
        raise ValueError('Cached input does not match the immutable public lock')
    return destination


def extract(asset, archive, destination):
    destination.mkdir(exist_ok=False)
    if asset['format'] == 'file':
        name = relative(urllib.parse.urlsplit(asset['url']).path.rsplit('/', 1)[1])
        shutil.copyfile(archive, destination / name)
        return destination / name
    seen = set()
    expanded = 0
    count = 0
    def write_entry(name, size, mode, incoming):
        nonlocal expanded, count
        name = relative(name)
        count += 1
        expanded += size
        if count > MAX_FILES or expanded > MAX_EXPANDED or size < 0:
            raise ValueError('Input expansion exceeds acquisition bounds')
        if name.casefold() in seen or mode & 0o170000 not in (0, 0o100000):
            raise ValueError('Duplicate or nonregular input entry')
        seen.add(name.casefold())
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open('xb') as outgoing:
            remaining = size
            while remaining:
                block = incoming.read(min(1024 * 1024, remaining))
                if not block:
                    raise ValueError('Truncated input member')
                outgoing.write(block)
                remaining -= len(block)
        target.chmod(0o755 if mode & 0o111 else 0o644)
    if asset['format'] == 'zip':
        with zipfile.ZipFile(archive) as stream:
            entries = stream.infolist()
            if len(entries) > MAX_FILES:
                raise ValueError('Input member count exceeds acquisition bounds')
            for item in entries:
                if item.is_dir():
                    relative(item.filename.rstrip('/'))
                    continue
                with stream.open(item) as incoming:
                    write_entry(item.filename, item.file_size, item.external_attr >> 16, incoming)
    else:
        with tarfile.open(archive, 'r|gz') as stream:
            for item in stream:
                if item.isdir():
                    relative(item.name.rstrip('/'))
                    count += 1
                    if count > MAX_FILES:
                        raise ValueError('Input member count exceeds acquisition bounds')
                    continue
                if not item.isfile():
                    raise ValueError('Input contains links/devices; publish a qualified regular-file dependency profile')
                with stream.extractfile(item) as incoming:
                    write_entry(item.name, item.size, item.mode, incoming)
    root = destination / asset['root'] if asset['root'] else destination
    if not root.is_dir():
        raise ValueError('Locked archive root is missing')
    return root


def validate_guest_release(document, profile):
    source = profile['guest_source_commit']
    tag = 'guest-tools-candidate-' + source
    expected = {'schema': 1, 'product': 'DockpipeGuestToolsCandidate', 'artifact_class': 'guest-tools',
                'version': '1.0.0', 'platform': 'windows-amd64', 'source_repository': REPOSITORY,
                'source_sha': source, 'source_state': 'committed-snapshot', 'tag': tag}
    if any(document.get(key) != value for key, value in expected.items()):
        raise ValueError('Guest Tools candidate is not the independently pinned committed Windows component')
    if document.get('checks') != ['native-windows-build', 'native-windows-ctest', 'installer-integrity']:
        raise ValueError('Guest Tools candidate lacks native qualification checks')
    toolchain = document.get('toolchain', {})
    expected_toolchain = {'qt_version': '6.10.3', 'compiler_id': 'GNU', 'compiler_version': '13.1.0',
                          'cmake_version': '3.30.5', 'ninja_version': '1.12.1'}
    if (set(toolchain) != set(expected_toolchain) | {'lock_sha256'}
            or any(toolchain.get(key) != value for key, value in expected_toolchain.items())
            or not re.fullmatch('[0-9a-f]{64}', toolchain.get('lock_sha256', ''))):
        raise ValueError('Guest Tools candidate lacks its exact qualified toolchain lock')
    manifest_url = 'https://github.com/' + REPOSITORY + '/releases/download/' + tag + '/guest-tools-candidate-manifest.json'
    if profile['inputs']['guest_release_manifest']['url'] != manifest_url:
        raise ValueError('Guest Tools candidate manifest must use its exact owning immutable release URL')
    names = {'guest_tools': 'dockpipe-guest-tools-1.0.0-windows-amd64.zip',
             'guest_descriptor': 'guest-tools-release.json',
             'guest_source_inventory': 'guest-tools-source-inventory.json',
             'guest_native_provenance': 'guest-tools-native-provenance.json',
             'guest_toolchain_lock': 'guest-tools-toolchain.lock.json'}
    artifacts = document.get('artifacts', {})
    if set(artifacts) != set(names):
        raise ValueError('Guest Tools qualification artifact set differs')
    prefix = 'https://github.com/' + REPOSITORY + '/releases/download/' + tag + '/'
    for role, name in names.items():
        artifact = artifacts[role]
        if set(artifact) != {'name', 'url', 'sha256', 'size_bytes'} or artifact['name'] != name or artifact['url'] != prefix + name:
            raise ValueError('Guest Tools qualification artifact source differs')
        validate_asset(dict(url=artifact['url'], sha256=artifact['sha256'], size_bytes=artifact['size_bytes'], format='file', root=''))
        if role in ('guest_tools', 'guest_descriptor'):
            selected = profile['inputs'][role]
            if any(selected[key] != artifact[key] for key in ('url', 'sha256', 'size_bytes')):
                raise ValueError('Guest Tools delivery input differs from its public qualification manifest')
    if artifacts['guest_toolchain_lock']['sha256'] != toolchain['lock_sha256']:
        raise ValueError('Guest Tools toolchain identity differs')


def prepare_runtime_root(work):
    """Use an empty private runtime store; no archive or ambient store is inherited."""
    root = work / 'runtime-global'
    if root.exists() or any(parent.is_symlink() for parent in (root, *root.parents)):
        raise ValueError('Self-contained runtime requires a fresh private store without links')
    root.mkdir(mode=0o700)
    return root


def validate_cli_source_pin(source, profile, target):
    """Bind the native producer to the exact released CLI in its owning source."""
    config = read_json(source / 'dockpipe.config.json')
    lock = read_json(source / 'dockpipe.lock.json')
    platform = target.replace('-', '/', 1)
    version = config.get('dockpipe_version')
    runtime = lock.get('dockpipe', {})
    artifact = runtime.get('artifacts', {}).get(platform, {})
    if (lock.get('schema') != 1 or not version or version != profile['cli_version']
            or runtime.get('version') != version):
        raise ValueError('Native CLI version differs from the owning public source lock')
    if any(artifact.get(key) != profile['inputs']['cli'][key] for key in ('url', 'sha256', 'size_bytes')):
        raise ValueError('Native CLI archive differs from the owning public source lock')


def combine(stores, owned, target, destination, previous):
    destination.mkdir(exist_ok=False)
    selected = {tuple(item) for item in owned}
    found = set()
    packages = {}
    for kind, directory in stores.items():
        document = read_json(directory / 'packages-store-manifest.json')
        if document.get('schema') != 1 or document.get('platform') != target or document.get('packages', {}).get('core'):
            raise ValueError('Store manifest has wrong platform or contains core')
        group = kind + 's'
        packages[group] = []
        for artifact in document.get('packages', {}).get(group, []):
            identity = (kind, artifact['name'])
            if identity not in selected:
                continue
            if identity in found:
                raise ValueError('Duplicate owned artifact')
            found.add(identity)
            name = relative(artifact['tarball'])
            if '/' in name or not name.startswith('dockpipe-') or not name.endswith('.tar.gz'):
                raise ValueError('Compiled artifact must have a flat versioned name')
            if artifact.get('variants'):
                raise ValueError('Variant archives need an explicitly qualified flat wire extension')
            source = directory / name
            if (source.is_symlink() or not source.is_file() or type(artifact.get('size_bytes')) is not int
                    or artifact['size_bytes'] <= 0 or source.stat().st_size != artifact['size_bytes']
                    or not re.fullmatch('[0-9a-f]{64}', artifact.get('sha256', ''))
                    or sha256(source) != artifact['sha256']):
                raise ValueError('Compiled artifact identity mismatch')
            if name in previous and previous[name] != artifact['sha256']:
                raise ValueError('Changed archive bytes require a new package version/name')
            if (destination / name).exists():
                raise ValueError('Artifact filename collision')
            shutil.copyfile(source, destination / name)
            packages[group].append(artifact)
    if found != selected:
        raise ValueError('Built identities differ from exact catalog ownership')
    result = {'schema': 1, 'platform': target, 'packages': packages}
    (destination / 'packages-store-manifest.json').write_text(json.dumps(result, sort_keys=True, indent=2) + '\n')
    return result


VM_FAMILY = {('resolver', 'qemu'), ('workflow', 'linux-vm'), ('workflow', 'windows-vm'),
             ('workflow', 'vm-manage'), ('workflow', 'vm-resolution')}


def verify_vm_family(manifest):
    found = set()
    for kind, group in (('workflow', 'workflows'), ('resolver', 'resolvers')):
        for artifact in manifest['packages'].get(group, []):
            identity = (kind, artifact['name'])
            if identity not in VM_FAMILY:
                continue
            if artifact.get('version') != '1.3.8' or artifact.get('min_dockpipe_version') != '0.6.29-0':
                raise ValueError('Native VM family requires coherent1.3.8/min0.6.29-0 metadata')
            found.add(identity)
    if found != VM_FAMILY:
        raise ValueError('Native candidate must contain all five VM family installables')


def archive_flat(directory, output):
    with output.open('xb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', mtime=0, filename='') as zipped, tarfile.open(fileobj=zipped, mode='w') as archive:
        for path in sorted(directory.iterdir()):
            if path.is_symlink() or not path.is_file():
                raise ValueError('Catalog archive must be flat regular files')
            data = path.read_bytes()
            info = tarfile.TarInfo(path.name)
            info.size = len(data)
            info.mode = 0o644
            archive.addfile(info, io.BytesIO(data))


def run(arguments, source, environment):
    subprocess.run([str(item) for item in arguments], cwd=source, env=environment, check=True, timeout=1800)


def build(lock_path, target, cache, work, output, fetch=False):
    lock, profile = load_lock(lock_path, target)
    if os.environ.get('GITHUB_SHA') and os.environ['GITHUB_SHA'] != lock['source_commit']:
        raise ValueError('CI checkout and locked source commit differ')
    actual = {'Linux': 'linux', 'Windows': 'windows', 'Darwin': 'darwin'}.get(platform.system(), '') + '-' + {'x86_64': 'amd64', 'AMD64': 'amd64', 'arm64': 'arm64'}.get(platform.machine(), '')
    if actual != target or work.exists() or output.exists():
        raise ValueError('Use an exact native host and fresh task work/output directories')
    work.mkdir(parents=True)
    output.mkdir(parents=True)
    global_root = prepare_runtime_root(work)
    inputs = {}
    for role, asset in profile['inputs'].items():
        inputs[role] = extract(asset, acquire(asset, cache, fetch), work / role)
    validate_guest_release(read_json(inputs['guest_release_manifest']), profile)
    recipe_environment = {}
    toolchain_receipt = None
    if 'toolchain_recipe' in profile:
        inputs['toolchain'], recipe_environment, toolchain_receipt = toolchain_inputs.prepare(
            inputs['source'], profile['toolchain_recipe'], target, work / 'prepared-toolchain',
            cache, fetch, dict(os.environ))
    if 'native_inputs' in profile:
        produced, recipe_environment = native_inputs.prepare(profile['native_inputs'], work / 'native-inputs',
            inputs['toolchain'], cache, fetch, acquire, recipe_environment or dict(os.environ))
        inputs.update(produced)
    source = inputs['source']
    cli = inputs['cli'] / ('dockpipe.exe' if target.startswith('windows-') else 'dockpipe')
    cli.chmod(0o755)
    environment = dict(os.environ)
    for key in list(environment):
        if key.startswith(('DOCKPIPE_', 'DYLD_', 'LD_LIBRARY_')):
            environment.pop(key)
    environment.update(DOCKPIPE_BIN=str(cli), DOCKPIPE_HOST_PYTHON_BIN=os.path.abspath(os.sys.executable),
                       DOCKPIPE_DISPLAY_SDK='display', DOCKPIPE_VM_DISPLAY_SDK_PREFIX=str(inputs['display_sdk']),
                       DOCKPIPE_VM_HOST_QT_PREFIX=str(inputs['qt']), DOCKPIPE_GIO_PREFIX=str(inputs['gio']),
                       DOCKPIPE_VM_DBUS_DAEMON_BIN=str(inputs['broker'] / ('dbus-daemon.exe' if target.startswith('windows-') else 'dbus-daemon')),
                       DOCKPIPE_VM_DBUS_PROVENANCE=str(inputs['broker_provenance']),
                       DOCKPIPE_VM_GUEST_TOOLS_ARCHIVE=str(inputs['guest_tools']),
                       DOCKPIPE_VM_GUEST_TOOLS_DESCRIPTOR=str(inputs['guest_descriptor']),
                       GOWORK='off', GOTOOLCHAIN='local')
    environment['DOCKPIPE_GLOBAL_ROOT'] = str(global_root)
    environment['DOCKPIPE_APPROVE_PROMPTS'] = '1'
    environment['DOCKPIPE_VM_BUILD_DIAGNOSTICS'] = str(output / 'diagnostics')
    sdk = work / 'sdk-inputs'
    sdk.mkdir()
    shutil.copyfile(inputs['go_sdk_archive'], sdk / 'sdk.tar.gz')
    shutil.copyfile(inputs['go_sdk_manifest'], sdk / 'sdk.json')
    dependencies = read_json(source / 'release/dependencies.lock.json')
    validate_cli_source_pin(source, profile, target)
    pin = dependencies['dockpipe_go_sdk']
    if sha256(sdk / 'sdk.tar.gz') != pin['archive_sha256'] or sha256(sdk / 'sdk.json') != pin['manifest_sha256']:
        raise ValueError('Go SDK acquisition differs from the catalog public dependency lock')
    environment['DOCKPIPE_GO_SDK_INPUTS'] = str(sdk)
    environment['PATH'] = os.pathsep.join(str(inputs[name] / 'bin') for name in ('toolchain', 'qt', 'gio')) + os.pathsep + os.defpath
    for key in ('CC', 'CXX', 'CMAKE_GENERATOR', 'CMAKE_PREFIX_PATH', 'PKG_CONFIG_SYSROOT_DIR', 'PKG_CONFIG_LIBDIR', 'LD_LIBRARY_PATH',
                'DOCKPIPE_VM_LINUX_DEPENDENCY_MODE', 'DOCKPIPE_VM_LINUX_PREFIX_RECEIPT', 'DOCKPIPE_VM_DISPLAY_INSPECTOR',
                'DOCKPIPE_NATIVE_LIB', 'DOCKPIPE_NATIVE_DUMPBIN', 'DOCKPIPE_NATIVE_RC', 'DOCKPIPE_NATIVE_MT', 'PATH', 'GOROOT'):
        if key in recipe_environment:
            environment[key] = recipe_environment[key]
    if target == 'darwin-arm64':
        for key in ('PKG_CONFIG', 'PKG_CONFIG_PATH', 'DOCKPIPE_VM_DISPLAY_BREW_PREFIX', 'SDKROOT', 'MACOSX_DEPLOYMENT_TARGET'):
            if key in recipe_environment:
                environment[key] = recipe_environment[key]
    actual_version = subprocess.check_output([str(cli), '--version'], env=environment, text=True, timeout=15).strip()
    if actual_version != profile['cli_version']:
        raise ValueError('CLI version differs from the public candidate lock')
    manifest = (source / 'packages/vm/package.yml').read_text()
    if not re.search(r'^\s+script: assets/scripts/build-source\.py\s*$', manifest, re.MULTILINE):
        raise ValueError('Approved catalog source must declare the native Python source hook')
    run([os.sys.executable, source / 'tooling/catalog_go_dependencies.py',
         '--sdk-inputs', sdk, '--target', target], source, environment)
    run([cli, 'package', 'build', 'source', '--workdir', source, '--only', 'vm'], source, environment)
    run([cli, 'package', 'compile', 'workflows', '--workdir', source, '--from', source / 'packages/vm/workflows', '--force'], source, environment)
    stores = {}
    for kind in ('workflow', 'resolver'):
        stores[kind] = work / (kind + '-store')
        run([cli, 'package', 'build', 'store', '--workdir', source, '--out', stores[kind], '--only', kind + 's'], source, environment)
    if 'compatibility_overlay' in profile:
        reference = profile['compatibility_overlay']
        archive = acquire(reference['asset'], cache, fetch)
        extracted = extract(reference['asset'], archive, work / 'compatibility-overlay')
        compatibility_overlay.merge(reference, extracted, stores['workflow'])
    flat = work / 'catalog'
    manifest = combine(stores, lock['owned'], target, flat, profile['previous_artifacts'])
    verify_vm_family(manifest)
    artifact = output / ('dockpipe-catalog-' + target + '.tar.gz')
    archive_flat(flat, artifact)
    descriptor = {'schema': 1, 'source_commit': lock['source_commit'], 'owned': lock['owned'], 'platform': target,
                  'repository': REPOSITORY, 'tag': catalog_transaction.release_tag('native', lock['source_commit']),
                  'publication_mode': 'native',
                  'asset': artifact.name, 'sha256': sha256(artifact), 'size_bytes': artifact.stat().st_size,
                  'manifest_sha256': sha256(flat / 'packages-store-manifest.json'),
                  'inputs_lock_sha256': sha256(lock_path), 'native_build': True, 'published': False,
                  'source_state': 'committed-snapshot', 'source_archive_sha256': profile['inputs']['source']['sha256'],
                  'checks': ['public-locked-inputs', 'owning-native-source-hook',
                             'native-display-build-and-ctest', 'exact-noncore-ownership', 'flat-store-integrity']}
    if toolchain_receipt is not None:
        receipt_copy = output / ('toolchain-receipt-' + target + '.json')
        shutil.copyfile(toolchain_receipt, receipt_copy)
        descriptor['toolchain_receipt_sha256'] = sha256(receipt_copy)
    (output / 'candidate.json').write_text(json.dumps(descriptor, indent=2) + '\n')
    return descriptor


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lock', type=Path, required=True)
    parser.add_argument('--platform', required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--fetch', action='store_true')
    args = parser.parse_args()
    build(args.lock, args.platform, args.cache.resolve(), args.work.resolve(), args.output.resolve(), args.fetch)


if __name__ == '__main__':
    main()
