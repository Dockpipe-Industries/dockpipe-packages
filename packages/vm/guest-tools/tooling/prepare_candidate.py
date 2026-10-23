#!/usr/bin/env python3
"""Prepare an immutable Guest Tools candidate; never upload or manufacture proof."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import tempfile

import materialize_bundle

REPOSITORY = 'Dockpipe-Industries/dockpipe-packages'
PRODUCT = 'DockpipeGuestToolsCandidate'
PLATFORM = 'windows-amd64'
MANIFEST_NAME = 'guest-tools-candidate-manifest.json'
NAMES = {
    'guest_tools': 'dockpipe-guest-tools-1.0.0-windows-amd64.zip',
    'guest_descriptor': 'guest-tools-release.json',
    'guest_source_inventory': 'guest-tools-source-inventory.json',
    'guest_native_provenance': 'guest-tools-native-provenance.json',
    'guest_toolchain_lock': 'guest-tools-toolchain.lock.json',
}
CHECKS = ['native-windows-build', 'native-windows-ctest', 'installer-integrity']
TOOL_VERSIONS = {
    'qt_version': '6.10.3',
    'compiler_id': 'GNU',
    'compiler_version': '13.1.0',
    'cmake_version': '3.30.5',
    'ninja_version': '1.12.1',
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def exact_fields(value, fields, label):
    if not isinstance(value, dict) or set(value) != set(fields):
        raise ValueError(label + ' has missing or unknown fields')


def hash_value(value):
    return isinstance(value, str) and re.fullmatch(r'[0-9a-f]{64}', value) is not None


def source_value(value):
    return isinstance(value, str) and re.fullmatch(r'[0-9a-f]{40}', value) is not None


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate metadata key')
        result[key] = value
    return result


def regular_bytes(path, maximum):
    if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= maximum:
        raise ValueError('Candidate input must be a bounded regular file: ' + path.name)
    return path.read_bytes()


def document(path, maximum=65536):
    data = regular_bytes(path, maximum)
    return json.loads(data, object_pairs_hook=unique_object), data


def source_inventory(value):
    if not isinstance(value, list) or not 0 < len(value) <= 512:
        raise ValueError('Source inventory exceeds its file bound')
    normalized = []
    seen = set()
    for entry in value:
        exact_fields(entry, {'path', 'sha256'}, 'Source inventory entry')
        name = entry['path']
        if not isinstance(name, str) or len(name.encode('utf-8')) > 512:
            raise ValueError('Invalid source inventory path')
        materialize_bundle.relative_name(name)
        if name.casefold() in seen or not hash_value(entry['sha256']):
            raise ValueError('Duplicate source path or invalid digest')
        seen.add(name.casefold())
        normalized.append({'path': name, 'sha256': entry['sha256']})
    ordered = sorted(normalized, key=lambda entry: tuple(part.casefold() for part in PurePosixPath(entry['path']).parts))
    if normalized != ordered:
        raise ValueError('Source inventory must retain the native Windows producer ordering')
    return normalized, digest(json.dumps(normalized, separators=(',', ':')).encode())


def validate_native_provenance(value, source_sha, archive_sha, descriptor_sha, inventory_sha, lock_sha):
    fields = {'schema', 'product', 'source_repository', 'source_sha', 'source_state',
              'native_host', 'qualification_origin', 'source_inventory_sha256',
              'archive_sha256', 'descriptor_sha256', 'toolchain_lock_sha256',
              'checks', 'ctest_tests', 'installer_integrity', 'workflow', *TOOL_VERSIONS}
    exact_fields(value, fields, 'Native provenance')
    expected = {'schema': 1, 'product': 'DockpipeGuestToolsNativeBuild',
                'source_repository': REPOSITORY, 'source_sha': source_sha,
                'source_state': 'committed-snapshot', 'native_host': PLATFORM,
                'qualification_origin': 'github-actions-native-windows',
                'source_inventory_sha256': inventory_sha, 'archive_sha256': archive_sha,
                'descriptor_sha256': descriptor_sha, 'toolchain_lock_sha256': lock_sha,
                **TOOL_VERSIONS}
    if type(value['schema']) is not int or any(value[key] != item for key, item in expected.items()):
        raise ValueError('Native provenance differs from committed source, artifacts, host, or toolchain')
    if value['checks'] != CHECKS or value['ctest_tests'] != {'guest-tools-status': 'passed', 'guest-tools-clipboard': 'passed'}:
        raise ValueError('Actual native build, both CTests, and installer rejection proof are required')
    integrity = value['installer_integrity']
    exact_fields(integrity, {'installer_parse', 'corrupt_executable_rejected', 'ui_task_unchanged'}, 'Installer integrity')
    if integrity != {'installer_parse': 'passed', 'corrupt_executable_rejected': True, 'ui_task_unchanged': True}:
        raise ValueError('Installer integrity proof failed')
    if type(integrity['corrupt_executable_rejected']) is not bool or type(integrity['ui_task_unchanged']) is not bool:
        raise ValueError('Installer outcomes require typed booleans')
    workflow = value['workflow']
    exact_fields(workflow, {'repository', 'run_id', 'run_attempt', 'workflow_sha'}, 'Hosted workflow identity')
    if (workflow['repository'] != REPOSITORY or type(workflow['run_id']) is not int
            or not 1 <= workflow['run_id'] < 2 ** 63
            or type(workflow['run_attempt']) is not int or not 1 <= workflow['run_attempt'] <= 100
            or not source_value(workflow['workflow_sha'])):
        raise ValueError('A bounded actual hosted workflow identity is required')


def validate_inputs(inputs, source_sha):
    if not source_value(source_sha):
        raise ValueError('Candidate source must be an exact lowercase 40-character commit')
    if not inputs.is_absolute() or inputs.is_symlink() or not inputs.is_dir():
        raise ValueError('Candidate inputs require an explicit regular directory')
    if {path.name for path in inputs.iterdir()} != set(NAMES.values()):
        raise ValueError('Candidate inputs require exactly the five declared assets')
    data = {role: regular_bytes(inputs / name, 256 << 20 if role == 'guest_tools' else 256 << 10)
            for role, name in NAMES.items()}
    descriptor, _ = document(inputs / NAMES['guest_descriptor'])
    descriptor_fields = {'schema', 'product', 'version', 'platform', 'qt_version', 'entrypoint',
                         'installer', 'media_subdirectory', 'archive', 'inventory_sha256', 'source_inventory_sha256'}
    exact_fields(descriptor, descriptor_fields, 'Delivery descriptor')
    if not hash_value(descriptor['inventory_sha256']) or not hash_value(descriptor['source_inventory_sha256']):
        raise ValueError('Delivery descriptor requires exact inventory digests')
    if type(descriptor['schema']) is not int:
        raise ValueError('Delivery schema requires a typed integer')
    inventory_value, _ = document(inputs / NAMES['guest_source_inventory'], 256 << 10)
    inventory, inventory_sha = source_inventory(inventory_value)
    if inventory_sha != descriptor['source_inventory_sha256']:
        raise ValueError('Source inventory differs from the actual native delivery descriptor')
    lock, _ = document(inputs / NAMES['guest_toolchain_lock'])
    lock_sha = digest(data['guest_toolchain_lock'])
    matching = [entry for entry in inventory if entry['path'] == 'tooling/toolchain.lock.json']
    if len(matching) != 1 or matching[0]['sha256'] != lock_sha:
        raise ValueError('Selected toolchain lock is not the qualified source inventory entry')
    if (type(lock.get('schema')) is not int or lock['schema'] != 1
            or lock.get('target') != PLATFORM or lock.get('qt_version') != TOOL_VERSIONS['qt_version']):
        raise ValueError('Unsupported Guest Tools toolchain lock')
    provenance, _ = document(inputs / NAMES['guest_native_provenance'])
    validate_native_provenance(provenance, source_sha, digest(data['guest_tools']),
                               digest(data['guest_descriptor']), inventory_sha, lock_sha)
    with tempfile.TemporaryDirectory(prefix='guest-candidate-check-') as temporary:
        directory = Path(temporary)
        archive = directory / NAMES['guest_tools']
        metadata = directory / NAMES['guest_descriptor']
        archive.write_bytes(data['guest_tools'])
        metadata.write_bytes(data['guest_descriptor'])
        materialize_bundle.materialize(archive, metadata, directory / 'bundle')
    return data, lock_sha


def manifest_from_bytes(data, lock_sha, source_sha):
    tag = 'guest-tools-candidate-' + source_sha
    prefix = 'https://github.com/' + REPOSITORY + '/releases/download/' + tag + '/'
    artifacts = {role: {'name': NAMES[role], 'url': prefix + NAMES[role],
                        'sha256': digest(value), 'size_bytes': len(value)} for role, value in data.items()}
    return {'schema': 1, 'product': PRODUCT, 'artifact_class': 'guest-tools', 'version': '1.0.0',
            'platform': PLATFORM, 'source_repository': REPOSITORY, 'source_sha': source_sha,
            'source_state': 'committed-snapshot', 'tag': tag, 'checks': CHECKS,
            'toolchain': {'lock_sha256': lock_sha, **TOOL_VERSIONS}, 'artifacts': artifacts}


def manifest_for(inputs, source_sha):
    data, lock_sha = validate_inputs(inputs, source_sha)
    return manifest_from_bytes(data, lock_sha, source_sha)


def validate_remote_release(release, tag_target, manifest, assets):
    source_sha = manifest['source_sha']
    if tag_target is not None and tag_target != source_sha:
        raise ValueError('Candidate tag points at a different source commit')
    if release is None:
        return 'create-draft'
    if not isinstance(release, dict):
        raise ValueError('Remote release requires an object')
    if tag_target is None and release.get('draft') is not True:
        raise ValueError('An existing release requires an exact resolved source tag')
    if (release.get('tag_name') != manifest['tag'] or release.get('target_commitish') != source_sha
            or release.get('prerelease') is not True or type(release.get('draft')) is not bool):
        raise ValueError('Existing release differs from the immutable candidate identity')
    remote = release.get('assets')
    if not isinstance(remote, list) or len(remote) != len(assets):
        raise ValueError('Remote asset inventory differs; never replace published bytes')
    by_name = {}
    for item in remote:
        if (not isinstance(item, dict) or not isinstance(item.get('name'), str)
                or item['name'] not in assets or item['name'] in by_name):
            raise ValueError('Remote release asset names are invalid or ambiguous')
        by_name[item.get('name')] = item
    if set(by_name) != set(assets):
        raise ValueError('Remote release asset inventory differs')
    for name, expected in assets.items():
        actual = by_name[name]
        if (type(actual.get('size')) is not int or actual['size'] != expected['size_bytes']
                or actual.get('digest') != 'sha256:' + expected['sha256']):
            raise ValueError('Remote asset bytes differ; never clobber or delete assets')
    return 'publish-qualified-draft' if release['draft'] else 'reuse-identical-candidate'


def prepare(inputs, output, source_sha):
    data, lock_sha = validate_inputs(inputs, source_sha)
    manifest = manifest_from_bytes(data, lock_sha, source_sha)
    if not output.is_absolute() or output.exists() or output.is_symlink():
        raise ValueError('Candidate preparation requires a fresh output directory')
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix='.guest-candidate-', dir=output.parent))
    try:
        assets_path = staging / 'assets'
        assets_path.mkdir()
        for role, value in data.items():
            (assets_path / NAMES[role]).write_bytes(value)
        manifest_bytes = (json.dumps(manifest, indent=2) + '\n').encode()
        (assets_path / MANIFEST_NAME).write_bytes(manifest_bytes)
        assets = {entry['name']: {'sha256': entry['sha256'], 'size_bytes': entry['size_bytes']}
                  for entry in manifest['artifacts'].values()}
        assets[MANIFEST_NAME] = {'sha256': digest(manifest_bytes), 'size_bytes': len(manifest_bytes)}
        plan = {'schema': 1, 'operation': 'prepare-only', 'repository': REPOSITORY,
                'tag': manifest['tag'], 'target_commitish': source_sha, 'draft': True,
                'prerelease': True, 'latest': False, 'assets': assets,
                'public_readback_required': True, 'publication_performed': False}
        (staging / 'publication-plan.json').write_text(json.dumps(plan, indent=2) + '\n')
        staging.rename(output)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source-sha', required=True)
    arguments = parser.parse_args()
    prepare(arguments.inputs, arguments.output, arguments.source_sha)
    print('Prepared exact candidate assets and plan; no publication performed.')


if __name__ == '__main__':
    main()
