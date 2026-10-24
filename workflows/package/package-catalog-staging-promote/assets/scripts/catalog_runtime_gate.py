"""Require the published runtime and compiled-core bytes used by candidate N."""
import hashlib
import json
from pathlib import Path
import re
import tarfile
import urllib.parse

from catalog_inputs import acquire_baseline, verified_json
from catalog_plan import relative_path
from catalog_transaction import PublicationStopped

ORIGIN = 'https://packages.staging.dockpipe.com/'
PLATFORMS = {'linux-amd64', 'windows-amd64', 'darwin-arm64'}


def admit_runtime(pin, source_lock, dependencies_lock, cli_pins, reader, root, acquire):
    root.mkdir(mode=0o700)
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
    manifest_path = acquire_baseline(pin['manifest'], root / 'manifest.json', reader)
    manifest = verified_json(manifest_path, pin['manifest'])
    if (manifest.get('artifact_class') != 'runtime' or manifest.get('source_sha') != pin['source_sha']
            or manifest.get('version') != pointer['version'] or pointer['version'] != pin['version']
            or set(pin['platforms']) != PLATFORMS):
        raise PublicationStopped('Published runtime source/version/platform identity differs')
    producer_core = pin['producer_core']
    expected_core_url = ('https://github.com/Dockpipe-Industries/dockpipe/releases/download/v'
                         + manifest['candidate'] + '/dockpipe-core-' + pin['version'] + '.tar.gz')
    if producer_core['url'] != expected_core_url:
        raise PublicationStopped('Producer core is not the exact owning runtime release asset')
    if any(dependencies_lock['dockpipe_runtime_core'][field] != producer_core[field]
           for field in ['url', 'sha256', 'size_bytes']):
        raise PublicationStopped('Product bootstrap dependencies lock still selects a different compiled core')
    producer_archive = acquire(producer_core, root / 'producer-cache')
    producer_script = vmimage_digest(producer_archive)
    result = {'source_sha': pin['source_sha'], 'version': pointer['version'], 'platforms': {}}
    for platform in sorted(PLATFORMS):
        selected = pin['platforms'][platform]
        store_url = urllib.parse.urljoin(expected_manifest, relative_path(manifest['stores'][platform]['manifest']))
        if selected['store']['url'] != store_url:
            raise PublicationStopped('Runtime platform manifest reference differs')
        store_path = acquire_baseline(selected['store'], root / (platform + '-store.json'), reader)
        store = verified_json(store_path, selected['store'])
        if store.get('platform') != platform or set(store.get('packages', {})) != {'core'}:
            raise PublicationStopped('Runtime store is not exact-platform core-only')
        core = store['packages']['core']
        core_pin = selected['core']
        if (core_pin['url'] != urllib.parse.urljoin(store_url, relative_path(core['tarball']))
                or any(core_pin[field] != core[field] for field in ['sha256', 'size_bytes'])):
            raise PublicationStopped('Runtime compiled-core archive reference differs')
        profile = source_lock['profiles'][platform]
        for role, expected in [('runtime_core', producer_core), ('cli', cli_pins[platform]['archive'])]:
            if any(profile['inputs'][role][field] != expected[field]
                   for field in ['url', 'sha256', 'size_bytes']):
                raise PublicationStopped('Native product source lock uses different CLI or compiled core')
        if profile['cli_version'] != cli_pins[platform]['version']:
            raise PublicationStopped('Native product exact CLI version differs')
        archive = acquire_baseline(core_pin, root / (platform + '-core.tar.gz'), reader)
        script_digest = vmimage_digest(archive)
        if script_digest != selected['vmimage_run_sha256'] or script_digest != producer_script:
            raise PublicationStopped('Published host and producer compiled-core vmimage scripts differ')
        result['platforms'][platform] = {'core_sha256': core_pin['sha256'],
                                          'vmimage_run_sha256': selected['vmimage_run_sha256']}
    return result


def vmimage_digest(archive):
    script = None
    with tarfile.open(archive, 'r:gz') as stream:
        for index, member in enumerate(stream):
            if index >= 10000:
                raise PublicationStopped('Runtime archive member capacity exceeded')
            if member.name != 'core/assets/scripts/vmimage-run.sh':
                continue
            if script is not None or not member.isfile() or member.size > 1048576:
                raise PublicationStopped('Runtime vmimage script is unsafe or duplicated')
            script = stream.extractfile(member).read(1048577)
    if script is None:
        raise PublicationStopped('Public compiled-core vmimage script is absent')
    return hashlib.sha256(script).hexdigest()
