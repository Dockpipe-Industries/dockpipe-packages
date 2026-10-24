"""Intake exact public archives using owning acquisition and catalog contracts."""
import hashlib
import json
from pathlib import Path
import tarfile
import urllib.parse

from catalog_declarations import read_declarations, dependency_closure
from catalog_plan import build_plan, entries, relative_path
from catalog_s3 import file_digest
from catalog_transaction import PublicationStopped


def verified_json(path, pin):
    digest, size = file_digest(path, pin['size_bytes'])
    if digest != pin['sha256'] or size != pin['size_bytes']:
        raise PublicationStopped('Public JSON input differs from exact pin')
    return json.loads(path.read_bytes())


def acquire_baseline(pin, target, reader):
    parsed = urllib.parse.urlsplit(pin['url'])
    if (parsed.scheme != 'https' or parsed.netloc != 'packages.staging.dockpipe.com'
            or parsed.query or parsed.fragment or parsed.username):
        raise PublicationStopped('Baseline input is outside the optional staging origin')
    observed = reader.fetch(relative_path(parsed.path.lstrip('/')), pin['size_bytes'], target)
    if (observed is None or observed.sha256 != pin['sha256']
            or observed.size_bytes != pin['size_bytes']):
        raise PublicationStopped('Exact public baseline input differs')
    return target


def unpack_native(archive_path, destination, pin):
    """Extract only the flat outer store, never any inner package payload."""
    destination.mkdir(mode=0o700)
    names = set()
    expanded = 0
    with tarfile.open(archive_path, 'r:gz') as stream:
        for index, member in enumerate(stream):
            expanded += member.size
            if (index >= 16 or expanded > 2 * 1024 * 1024 * 1024
                    or not member.isfile() or '/' in member.name or '\\' in member.name
                    or member.name in names or not 0 < member.size <= 512 * 1024 * 1024):
                raise PublicationStopped('Native outer archive is not a bounded flat regular-file store')
            relative_path(member.name)
            names.add(member.name)
            with stream.extractfile(member) as source, (destination / member.name).open('xb') as target:
                remaining = member.size
                while remaining:
                    block = source.read(min(1024 * 1024, remaining))
                    if not block:
                        raise PublicationStopped('Native outer store member is truncated')
                    target.write(block)
                    remaining -= len(block)
    manifest_path = destination / 'packages-store-manifest.json'
    if file_digest(manifest_path, 1048576)[0] != pin['manifest_sha256']:
        raise PublicationStopped('Native platform manifest differs from admitted public pin')
    manifest = json.loads(manifest_path.read_bytes())
    selected = entries(manifest)
    if names != {'packages-store-manifest.json'} | {item['tarball'] for item in selected.values()}:
        raise PublicationStopped('Native outer archive has extra or missing members')
    for item in selected.values():
        digest, size = file_digest(destination / item['tarball'], item['size_bytes'])
        if digest != item['sha256'] or size != item['size_bytes']:
            raise PublicationStopped('Native inner archive differs from admitted manifest')
    return manifest


def prepare(request, output, native_acquire, reader):
    """native_acquire is the existing owning exact-public-input acquisition helper."""
    output = Path(output)
    if not output.is_absolute() or output.exists():
        raise PublicationStopped('Input output must be an absolute fresh private root')
    output.mkdir(mode=0o700)
    cache = output / 'cache'
    cache.mkdir(mode=0o700)
    baselines = {}
    candidates = {}
    locations = {}
    for platform, pin in request['baseline_manifests'].items():
        target = output / ('baseline-' + platform + '.json')
        acquire_baseline(pin, target, reader)
        baselines[platform] = verified_json(target, pin)
    for platform, pin in request['candidate']['artifacts'].items():
        path = native_acquire(pin, cache)
        candidates[platform] = unpack_native(path, output / ('native-' + platform), pin)
    metadata = {}
    closures = {}
    for platform, policy in request['promotion']['policy'].items():
        available = entries(baselines[platform])
        native = entries(candidates[platform]) if platform in candidates else {}
        selected = {key: value for key, value in available.items() if key not in request['promotion']['owned']}
        for key, decision in policy.items():
            if decision['action'] == 'promote':
                selected[key] = native[key]
            elif decision['action'] == 'retain-baseline':
                selected[key] = available[key]
        metadata[platform] = {}
        locations[platform] = {}
        pending = [key for key, decision in policy.items() if decision['action'] == 'promote']
        while pending:
            key = pending.pop()
            if key in metadata[platform]:
                continue
            if len(metadata[platform]) >= 64:
                raise PublicationStopped('Compiled dependency metadata exceeds selected closure capacity')
            entry = selected.get(key)
            if entry is None:
                raise PublicationStopped('Selected dependency is absent from exact platform catalog')
            if key in policy and policy[key]['action'] == 'promote':
                archive = output / ('native-' + platform) / entry['tarball']
            else:
                parent_url = request['baseline_manifests'][platform]['url']
                pin = {**entry, 'url': urllib.parse.urljoin(parent_url, entry['tarball'])}
                archive = cache / entry['sha256']
                if not archive.exists():
                    acquire_baseline(pin, archive, reader)
            kind, name = key.split('/', 1)
            declaration = read_declarations(archive, kind, name, entry)
            metadata[platform][key] = declaration
            locations[platform][key] = str(archive.relative_to(output))
            resolvers = set(declaration['requires_resolvers'])
            for dependency in set(declaration['depends']) | resolvers:
                matches = [candidate for candidate in selected if
                           candidate == 'resolver/' + dependency] if dependency in resolvers else [
                               candidate for candidate in selected if candidate.split('/', 1)[1] == dependency]
                if len(matches) != 1:
                    raise PublicationStopped('Declared dependency is missing or ambiguous')
                pending.append(matches[0])
        closures[platform] = {key: dependency_closure(key, selected, metadata[platform])
                              for key, decision in policy.items() if decision['action'] == 'promote'}
    plan = build_plan(request['promotion'], baselines, candidates, metadata)
    plan['dependency_closures'] = closures
    plan['input_manifests'] = {platform: json.loads(item['bytes']) for platform in baselines
                               for item in plan['objects'] if item['role'] == 'store-manifest'
                               and '/stores/' + platform + '/' in item['key']}
    plan['baseline_pointer'] = request['baseline_pointer']
    plan['cli'] = request['cli']
    plan['archive_sources'] = {}
    for item in plan['objects']:
        if item['role'] != 'archive':
            continue
        platform = item['platform']
        key = item['identity']
        if key in locations[platform]:
            plan['archive_sources'][item['key']] = {'input_file': locations[platform][key]}
        else:
            url = urllib.parse.urljoin(request['baseline_manifests'][platform]['url'], item['key'].rsplit('/', 1)[1])
            plan['archive_sources'][item['key']] = {'url': url}
    return plan
