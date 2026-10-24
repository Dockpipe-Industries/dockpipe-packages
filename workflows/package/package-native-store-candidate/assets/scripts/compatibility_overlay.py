"""Preserve the qualified source-only repair bytes in a later native union store."""
import json
from pathlib import Path
import re
import shutil

import native_inputs

OWNED = [['workflow', 'dockpipe.terraform.core'], ['workflow', 'dockpipe.cloudflare.r2infra'],
         ['workflow', 'secretstore-r2-publish-test']]


def validate(reference, validate_asset):
    if (not isinstance(reference, dict) or set(reference) != {'source_commit', 'owned', 'asset'}
            or not isinstance(reference['source_commit'], str)
            or not re.fullmatch('[0-9a-f]{40}', reference['source_commit'])
            or reference['owned'] != OWNED):
        raise ValueError('Compatibility overlay requires the exact qualified three-workflow source identity')
    asset = reference['asset']
    validate_asset(asset)
    expected = ('https://github.com/Dockpipe-Industries/dockpipe-packages/releases/download/catalog-candidate-'
                + reference['source_commit'] + '/dockpipe-catalog-linux-amd64.tar.gz')
    if asset['url'] != expected or asset['format'] != 'tar.gz' or asset['root'] != '':
        raise ValueError('Compatibility overlay must retain its source-bound immutable release wire')


def merge(reference, extracted, workflow_store):
    manifest_path = extracted / 'packages-store-manifest.json'
    if manifest_path.stat().st_size > 1 << 20:
        raise ValueError('Compatibility manifest exceeds its bound')
    manifest = json.loads(manifest_path.read_bytes(), object_pairs_hook=native_inputs.unique_fields)
    packages = manifest.get('packages', {})
    if (manifest.get('schema') != 1 or manifest.get('platform') != 'linux-amd64'
            or set(packages) - {'workflows', 'resolvers'} or packages.get('resolvers')):
        raise ValueError('Compatibility input is not its qualified source-only flat store')
    entries = packages.get('workflows')
    if not isinstance(entries, list) or len(entries) != 3:
        raise ValueError('Compatibility workflow set differs')
    actual = []
    paths = {'packages-store-manifest.json'}
    for entry in entries:
        if not isinstance(entry, dict):
            raise ValueError('Compatibility package metadata is invalid')
        name = entry.get('tarball')
        if not isinstance(name, str) or not re.fullmatch(r'dockpipe-workflow-[A-Za-z0-9_.-]+\.tar\.gz', name):
            raise ValueError('Compatibility archive filename is invalid')
        native_inputs.relative(name)
        path = extracted / name
        if (name in paths or path.is_symlink() or not path.is_file()
                or type(entry.get('size_bytes')) is not int or not 0 < entry['size_bytes'] <= 64 << 20
                or path.stat().st_size != entry['size_bytes'] or native_inputs.digest(path) != entry.get('sha256')):
            raise ValueError('Compatibility archive differs from qualified bytes')
        paths.add(name)
        actual.append(['workflow', entry.get('name')])
    if sorted(actual) != sorted(reference['owned']) or {path.name for path in extracted.iterdir()} != paths:
        raise ValueError('Compatibility input ownership/file inventory differs')
    destination_path = workflow_store / 'packages-store-manifest.json'
    destination = json.loads(destination_path.read_bytes(), object_pairs_hook=native_inputs.unique_fields)
    workflows = destination['packages'].setdefault('workflows', [])
    if any(['workflow', entry['name']] in actual for entry in workflows):
        raise ValueError('Native and source-compatibility ownership overlap')
    for entry in entries:
        target = workflow_store / entry['tarball']
        if target.exists():
            raise ValueError('Compatibility archive would replace an existing artifact')
    for entry in entries:
        shutil.copyfile(extracted / entry['tarball'], workflow_store / entry['tarball'])
        workflows.append(entry)
    destination_path.write_text(json.dumps(destination, indent=2) + '\n')
