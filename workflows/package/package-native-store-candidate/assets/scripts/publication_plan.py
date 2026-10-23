"""Validate completed native candidates and prepare an immutable publication plan. No network mutation."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tarfile
import native_store


def plan(candidate_paths, *, qualification="native"):
    if qualification not in {"native", "source-only"}:
        raise ValueError("Unsupported publication qualification class")
    artifacts = {}
    source = None
    owned = None
    for candidate_path in candidate_paths:
        descriptor = native_store.read_json(candidate_path)
        target = descriptor.get('platform')
        commit = descriptor.get('source_commit')
        if (descriptor.get('schema') != 1 or target not in native_store.SUPPORTED
                or not isinstance(commit, str) or not re.fullmatch('[0-9a-f]{40}', commit)
                or descriptor.get('native_build') is not (qualification == 'native')
                or (qualification == 'source-only' and descriptor.get('source_only') is not True)
                or descriptor.get('published') is not False
                or descriptor.get('repository') != native_store.REPOSITORY
                or descriptor.get('tag') != 'catalog-candidate-' + commit):
            raise ValueError('Unsupported native candidate publication descriptor')
        if source is not None and (source != commit or owned != descriptor['owned']):
            raise ValueError('Publication candidates differ in source or ownership')
        source, owned = commit, descriptor['owned']
        if target in artifacts:
            raise ValueError('Duplicate native platform')
        name = 'dockpipe-catalog-' + target + '.tar.gz'
        archive = candidate_path.parent / name
        if descriptor.get('asset') != name or archive.is_symlink() or not archive.is_file() or archive.stat().st_size != descriptor['size_bytes'] or native_store.sha256(archive) != descriptor['sha256']:
            raise ValueError('Native candidate archive integrity mismatch')
        with tarfile.open(archive) as stream:
            members = []
            expanded = 0
            for item in stream:
                expanded += item.size
                if len(members) >= 64 or item.size < 0 or expanded > 512 * 1024 * 1024:
                    raise ValueError('Native store expansion exceeds publication bounds')
                members.append(item)
            if any(not item.isfile() or '/' in item.name or native_store.relative(item.name) != item.name for item in members):
                raise ValueError('Native candidate must contain flat regular files')
            names = [item.name for item in members]
            if len(set(names)) != len(names):
                raise ValueError('Duplicate native candidate member')
            member = stream.getmember('packages-store-manifest.json')
            if member.size > 1024 * 1024:
                raise ValueError('Native store manifest exceeds metadata bounds')
            manifest_bytes = stream.extractfile(member).read()
            if hashlib.sha256(manifest_bytes).hexdigest() != descriptor['manifest_sha256']:
                raise ValueError('Native store manifest identity mismatch')
            manifest = json.loads(manifest_bytes)
            if manifest.get('schema') != 1 or manifest.get('platform') != target or set(manifest['packages']) - {'workflows', 'resolvers'}:
                raise ValueError('Native store platform/core boundary mismatch')
            actual_owned = []
            referenced = {'packages-store-manifest.json'}
            for kind, group in (('workflow', 'workflows'), ('resolver', 'resolvers')):
                for entry in manifest['packages'].get(group, []):
                    actual_owned.append([kind, entry['name']])
                    name = entry['tarball']
                    if (not name.startswith('dockpipe-') or not name.endswith('.tar.gz')
                            or '/' in name or native_store.relative(name) != name):
                        raise ValueError('Package archive must use the flat versioned Dockpipe wire')
                    if name in referenced:
                        raise ValueError('Duplicate referenced package archive')
                    referenced.add(name)
                    item = stream.getmember(name)
                    if item.size != entry['size_bytes'] or item.size <= 0:
                        raise ValueError('Package archive size differs')
                    digest = hashlib.sha256()
                    with stream.extractfile(item) as incoming:
                        for block in iter(lambda: incoming.read(1024 * 1024), b''):
                            digest.update(block)
                    if digest.hexdigest() != entry['sha256']:
                        raise ValueError('Package archive hash differs')
            if sorted(actual_owned) != sorted(owned) or set(names) != referenced:
                raise ValueError('Archive differs from exact package ownership/file set')
        artifacts[target] = {key: descriptor[key] for key in ('sha256', 'size_bytes', 'manifest_sha256')}
        artifacts[target]['url'] = ('https://github.com/' + native_store.REPOSITORY + '/releases/download/catalog-candidate-' + source + '/' + archive.name)
    if not artifacts:
        raise ValueError('At least one actually built native platform is required')
    return {'schema': 1, 'repository': native_store.REPOSITORY, 'tag': 'catalog-candidate-' + source,
            'source_commit': source, 'owned': owned, 'artifacts': artifacts,
            'publication': 'review required; create immutable prerelease, never overwrite existing assets or update latest',
            'core_lock_overlay': {'source_commit': source, 'owned': owned, 'artifacts': artifacts}}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, action='append', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    with args.output.open('x') as output:
        json.dump(plan(args.candidate), output, indent=2)
        output.write('\n')
