"""Bind the public runtime/CLI/SDK and the actual package-owned VM adapter."""
import hashlib
import re
import tarfile
import urllib.parse

from catalog_inputs import acquire_baseline, verified_json
from catalog_plan import entries, relative_path
from catalog_s3 import file_digest
from catalog_transaction import PublicationStopped

ORIGIN = 'https://packages.staging.dockpipe.com/'
RELEASES = 'https://github.com/Dockpipe-Industries/dockpipe/releases/download/'
PLATFORMS = {'linux-amd64', 'windows-amd64', 'darwin-arm64'}


def metadata_pin(pin, maximum):
    if (not isinstance(pin, dict) or type(pin.get('size_bytes')) is not int
            or not 0 < pin['size_bytes'] <= maximum
            or not re.fullmatch('[0-9a-f]{64}', pin.get('sha256', ''))):
        raise PublicationStopped('Public metadata requires its exact bounded byte identity')


def admit_cli_manifest(pin, cli_pins, acquire, root):
    metadata_pin(pin, 128 * 1024)
    if not re.fullmatch(re.escape(RELEASES) + r'cli-candidate-[0-9]+\.[0-9]+\.[0-9]+-staging\.[0-9a-f]{40}/cli-candidate\.json', pin.get('url', '')):
        raise PublicationStopped('CLI metadata must come from its exact owning public release')
    document = verified_json(acquire(pin, root / 'public-input-cache'), pin)
    source_sha = document.get('source_sha', '')
    version = document.get('version', '')
    tag = document.get('tag', '')
    selected_version = re.fullmatch(r'([0-9]+\.[0-9]+\.[0-9]+)-staging\.cli\.sha([0-9a-f]{12})', version)
    if (document.get('schema') != 1 or document.get('artifact_class') != 'cli-only'
            or document.get('source_repository') != 'Dockpipe-Industries/dockpipe'
            or document.get('source_state') != 'committed-snapshot'
            or not re.fullmatch('[0-9a-f]{40}', source_sha)
            or selected_version is None
            or tag != 'cli-candidate-' + selected_version.group(1) + '-staging.' + source_sha
            or not version.endswith('.sha' + source_sha[:12])
            or pin['url'] != RELEASES + tag + '/cli-candidate.json'
            or set(cli_pins) != PLATFORMS):
        raise PublicationStopped('Public CLI manifest source or release identity differs')
    if set(document.get('artifacts', {})) != {'linux/amd64', 'linux/arm64', 'darwin/amd64', 'darwin/arm64', 'windows/amd64'}:
        raise PublicationStopped('Public CLI producer manifest must retain all five exact platforms')
    required_checks = {'public-module-download', 'module-integrity', 'pure-go-five-platform-build',
                       'native-linux-version', 'native-python-package-source-hook', 'native-python-host-step'}
    if not required_checks.issubset(document.get('checks', [])):
        raise PublicationStopped('Public CLI lacks its owning producer qualification')
    for platform, selected in cli_pins.items():
        artifact = document.get('artifacts', {}).get(platform.replace('-', '/', 1), {})
        expected_url = RELEASES + tag + '/' + artifact.get('name', '')
        if (selected.get('version') != version or selected['archive'].get('url') != expected_url
                or any(selected['archive'].get(key) != artifact.get(key) for key in ('sha256', 'size_bytes'))
                or any(selected.get(key) != artifact.get(key) for key in ('executable_sha256', 'executable_size_bytes'))):
            raise PublicationStopped('CLI executable/archive differs from its exact public producer manifest')
    return document


def admit_sdk(profile, dependencies, acquire, root):
    pin = dependencies.get('dockpipe_go_sdk', {})
    identity = pin.get('source_digest', '')
    if (not re.fullmatch('[0-9a-f]{64}', identity)
            or not re.fullmatch('[0-9a-f]{40}', pin.get('source_commit', ''))
            or pin.get('version') != '0.1.0-source.' + identity[:16]):
        raise PublicationStopped('Owning SDK dependency source identity is incomplete')
    prefix = RELEASES + 'dockpipe-go-sdk-' + pin['version'] + '/dockpipe-go-sdk-' + identity[:16]
    for role, suffix, checksum in [('go_sdk_archive', '.tar.gz', 'archive_sha256'),
                                   ('go_sdk_manifest', '.json', 'manifest_sha256')]:
        selected = profile['inputs'][role]
        if selected['url'] != prefix + suffix or selected['sha256'] != pin.get(checksum):
            raise PublicationStopped('Native product SDK differs from the owning public dependency pin')
    reference = profile['inputs']['go_sdk_manifest']
    metadata_pin(reference, 4 * 1024 * 1024)
    manifest = verified_json(acquire(reference, root / 'public-input-cache'), reference)
    expected = {'schema': 1, 'module': 'dockpipe', 'sdk_version': pin['version'],
                'source_commit': pin['source_commit'], 'source_digest': identity,
                'sha256': pin['archive_sha256'], 'bytes': profile['inputs']['go_sdk_archive']['size_bytes']}
    if any(manifest.get(key) != value for key, value in expected.items()):
        raise PublicationStopped('Published SDK manifest does not bind the selected source/archive')
    return pin['manifest_sha256']


def admit_runtime(pin, source_lock, dependencies_lock, cli_pins, reader, root, acquire, package_stores):
    root.mkdir(mode=0o700)
    metadata_pin(pin['pointer'], 1048576)
    if pin['pointer']['url'] != ORIGIN + 'packages/runtime/latest.json':
        raise PublicationStopped('Runtime readiness pointer is not the separate staging runtime pointer')
    pointer_path = acquire_baseline(pin['pointer'], root / 'pointer.json', reader)
    pointer = verified_json(pointer_path, pin['pointer'])
    version = re.match(r'^([0-9]+)\.([0-9]+)\.([0-9]+)(?:-|$)', pointer.get('version', ''))
    if not version or tuple(map(int, version.groups())) < (0, 6, 28):
        raise PublicationStopped('Published runtime is below the required Darwin runtime floor')
    expected_manifest = ORIGIN + relative_path(pointer['manifest'])
    if pin['manifest']['url'] != expected_manifest:
        raise PublicationStopped('Runtime pointer does not select the exact admitted manifest')
    metadata_pin(pin['manifest'], 1048576)
    manifest_path = acquire_baseline(pin['manifest'], root / 'manifest.json', reader)
    manifest = verified_json(manifest_path, pin['manifest'])
    if (manifest.get('artifact_class') != 'runtime' or manifest.get('source_sha') != pin['source_sha']
            or manifest.get('version') != pointer['version'] or pointer['version'] != pin['version']
            or set(pin['platforms']) != PLATFORMS or set(package_stores) != PLATFORMS
            or 'dockpipe_runtime_core' in dependencies_lock):
        raise PublicationStopped('Published runtime source/version/platform or independent product dependencies differ')
    if set(pin) != {'pointer', 'manifest', 'source_sha', 'version', 'platforms', 'cli_manifest'}:
        raise PublicationStopped('Runtime readiness contract has missing or obsolete fields')
    cli_manifest = admit_cli_manifest(pin['cli_manifest'], cli_pins, acquire, root)
    result = {'source_sha': pin['source_sha'], 'version': pointer['version'],
              'cli_source_sha': cli_manifest['source_sha'], 'cli_manifest_sha256': pin['cli_manifest']['sha256'],
              'platforms': {}}
    for platform in sorted(PLATFORMS):
        selected = pin['platforms'][platform]
        if set(selected) != {'vmimage_run_sha256'} or not re.fullmatch('[0-9a-f]{64}', selected['vmimage_run_sha256']):
            raise PublicationStopped('Exact reviewed package-owned VM adapter identity is required')
        profile = source_lock['profiles'][platform]
        if (profile['cli_version'] != cli_pins[platform]['version']
                or any(profile['inputs']['cli'][field] != cli_pins[platform]['archive'][field]
                       for field in ('url', 'sha256', 'size_bytes'))
                or 'runtime_core' in profile['inputs']):
            raise PublicationStopped('Native product exact public CLI identity differs or still requires core')
        sdk_digest = admit_sdk(profile, dependencies_lock, acquire, root)
        reference = package_stores[platform]
        store = reference['directory']
        catalog = verified_package_manifest(store, reference['manifest_sha256'])
        if catalog.get('platform') != platform or 'core' in catalog.get('packages', {}):
            raise PublicationStopped('Native product must retain its exact noncore platform ownership')
        package = entries(catalog).get('resolver/qemu')
        if package is None:
            raise PublicationStopped('Native product has no owning QEMU resolver package')
        relative_path(package['tarball'])
        archive = store / package['tarball']
        if archive.is_symlink() or not archive.is_file() or file_digest(archive, package['size_bytes']) != (package['sha256'], package['size_bytes']):
            raise PublicationStopped('Owning QEMU resolver archive differs from the admitted native product')
        script_digest = vmimage_digest(archive)
        if script_digest != selected['vmimage_run_sha256']:
            raise PublicationStopped('Actual package-owned VM adapter differs from the reviewed effective script')
        result['platforms'][platform] = {'resolver_sha256': package['sha256'],
                                         'sdk_manifest_sha256': sdk_digest,
                                         'vmimage_run_sha256': script_digest}
    return result


def verified_package_manifest(store, expected_digest):
    path = store / 'packages-store-manifest.json'
    if path.is_symlink() or not path.is_file():
        raise PublicationStopped('Native product manifest is not a regular file')
    digest, size = file_digest(path, 1048576)
    if digest != expected_digest:
        raise PublicationStopped('Native product manifest differs from its public candidate pin')
    return verified_json(path, {'sha256': digest, 'size_bytes': size})


def vmimage_digest(archive):
    script = None
    with tarfile.open(archive, 'r:gz') as stream:
        expanded = 0
        for index, member in enumerate(stream):
            expanded += member.size
            if index >= 10000 or member.size < 0 or expanded > 2 * 1024 * 1024 * 1024:
                raise PublicationStopped('Resolver archive member capacity exceeded')
            if member.name != 'resolvers/qemu/assets/scripts/vmimage-run.sh':
                continue
            if script is not None or not member.isfile() or member.size > 1048576:
                raise PublicationStopped('Package-owned VM adapter is unsafe or duplicated')
            script = stream.extractfile(member).read(1048577)
    if script is None:
        raise PublicationStopped('Actual QEMU resolver VM adapter is absent')
    return hashlib.sha256(script).hexdigest()
