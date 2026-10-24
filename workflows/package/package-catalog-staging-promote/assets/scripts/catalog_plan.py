"""Compose an optional catalog from admitted immutable inputs and explicit ownership decisions."""
import copy
import hashlib
import json
from pathlib import PurePosixPath
import re

BASELINE_PLATFORMS = {'linux-amd64', 'linux-arm64', 'darwin-amd64', 'darwin-arm64',
                      'windows-amd64', 'linux-amd64-flatpak-org.kde.Platform-6.10'}
NATIVE_PLATFORMS = {'linux-amd64', 'windows-amd64', 'darwin-arm64'}
MAX_PLATFORMS = 6
MAX_ENTRIES = 1000
MAX_ARCHIVE_BYTES = 512 * 1024 * 1024
MAX_TOTAL_BYTES = 8 * 1024 * 1024 * 1024
SHA256 = re.compile(r'[0-9a-f]{64}')
COMMIT = re.compile(r'[0-9a-f]{40}')
TOKEN = re.compile(r'[A-Za-z0-9][A-Za-z0-9._+-]*')


def json_bytes(value):
    return (json.dumps(value, sort_keys=True, indent=2) + '\n').encode()


def relative_path(value):
    if (not isinstance(value, str) or not value or len(value) > 512
            or any(character in value for character in '\\:%?#\x00\r\n')
            or value.startswith('/') or PurePosixPath(value).as_posix() != value
            or any(part in {'', '.', '..'} for part in value.split('/'))):
        raise ValueError('Catalog reference must be an ordinary contained relative path')
    return value


def identity(kind, name):
    if kind not in {'resolver', 'workflow'} or not isinstance(name, str) or not TOKEN.fullmatch(name) or '..' in name:
        raise ValueError('Invalid optional package identity')
    return kind + '/' + name


def entries(manifest):
    if manifest.get('schema') != 1 or not isinstance(manifest.get('packages'), dict):
        raise ValueError('Store schema is invalid')
    if set(manifest['packages']) - {'core', 'resolvers', 'workflows'}:
        raise ValueError('Unknown store package group')
    result = {}
    for kind, group in [('resolver', 'resolvers'), ('workflow', 'workflows')]:
        items = manifest['packages'].get(group, [])
        if not isinstance(items, list):
            raise ValueError('Store package group must be a list')
        for item in items:
            key = identity(kind, item.get('name'))
            version = item.get('version')
            if not isinstance(version, str) or not TOKEN.fullmatch(version) or '..' in version:
                raise ValueError('Unsafe package version')
            expected = 'dockpipe-' + kind + '-' + item['name'] + '-' + item.get('version', '') + '.tar.gz'
            if (key in result or item.get('tarball') != expected
                    or not SHA256.fullmatch(item.get('sha256', ''))
                    or type(item.get('size_bytes')) is not int
                    or not 0 < item['size_bytes'] <= MAX_ARCHIVE_BYTES):
                raise ValueError('Duplicate, unsafe or unbounded package entry')
            result[key] = copy.deepcopy(item)
    if len(result) > MAX_ENTRIES:
        raise ValueError('Store entry capacity exceeded')
    return result


def host_platform(platform):
    if '-flatpak-' in platform:
        return 'flatpak'
    for prefix, host in [('linux-', 'deb'), ('windows-', 'windows'), ('darwin-', 'macos')]:
        if platform.startswith(prefix):
            return host
    raise ValueError('Unsupported catalog platform')


def require_platform(metadata, platform):
    host = host_platform(platform)
    for field in ['package_platforms', 'workflow_platforms']:
        declared = metadata.get(field)
        if declared and host not in declared and not (host == 'deb' and 'linux' in declared):
            raise ValueError('New package does not declare this host platform')
    implicit = metadata.get('resolver')
    if implicit and implicit not in metadata.get('requires_resolvers', []):
        raise ValueError('New workflow has an undeclared resolver installation dependency')


def require_dependencies(metadata, available):
    required_resolvers = set(metadata.get('requires_resolvers', []))
    names = set(metadata.get('depends', [])) | required_resolvers
    for name in names:
        if name in required_resolvers:
            matches = [key for key in available if key == 'resolver/' + name]
        else:
            matches = [key for key in available if key.split('/', 1)[1] == name]
        if len(matches) != 1:
            raise ValueError('Required package is missing or ambiguous: ' + name)


def compose_platform(platform, baseline, candidate, owned, policy, metadata):
    if baseline.get('platform') != platform or set(policy) != set(owned):
        raise ValueError('Exact platform and explicit decisions for every owned identity are required')
    previous = entries(baseline)
    incoming = entries(candidate) if candidate is not None else {}
    result = {key: value for key, value in previous.items() if key not in owned}
    decisions = []
    promoted = []
    for key in sorted(owned):
        item = policy[key]
        action = item.get('action')
        reason = item.get('reason')
        if not isinstance(reason, str) or not reason.strip() or len(reason) > 1024:
            raise ValueError('Every promotion, retention or retirement needs a bounded explicit reason')
        if action == 'promote':
            if candidate is None or candidate.get('platform') != platform or key not in incoming:
                raise ValueError('New package has no matching qualified native-platform artifact')
            if key not in metadata:
                raise ValueError('New package needs verified compiled declaration metadata')
            require_platform(metadata[key], platform)
            result[key] = incoming[key]
            promoted.append(key)
        elif action == 'retain-baseline':
            if key not in previous:
                raise ValueError('Cannot retain an absent baseline package')
            result[key] = previous[key]
        elif action != 'retire':
            raise ValueError('Unknown ownership decision')
        decisions.append({'identity': key, 'action': action, 'reason': reason,
                          'previous': previous.get(key), 'selected': result.get(key)})
    for key in promoted:
        require_dependencies(metadata[key], result)
    manifest = {'schema': 1, 'platform': platform,
                'packages': {'resolvers': [], 'workflows': []}}
    for key, entry in sorted(result.items()):
        group = 'resolvers' if key.startswith('resolver/') else 'workflows'
        manifest['packages'][group].append(entry)
    # Required runtime core is intentionally not in this optional producer's authority.
    return manifest, {'core_excluded': baseline['packages'].get('core') is not None,
                      'decisions': decisions,
                      'unrelated_retained': sorted(key for key in previous if key not in owned),
                      'uses_admitted_candidate': candidate is not None}


def build_plan(request, baselines, candidates, metadata):
    source = request.get('product_source_sha', '')
    orchestration = request.get('orchestration_source_sha', '')
    if (not COMMIT.fullmatch(source) or not COMMIT.fullmatch(orchestration)
            or request.get('product_tag') != 'catalog-native-candidate-' + source
            or request.get('channel') != 'staging'
            or request.get('public_origin') != 'https://packages.staging.dockpipe.com'
            or request.get('exclude_runtime_core') is not True):
        raise ValueError('Exact product/orchestration identities and staging destination are required')
    owned = request.get('owned')
    if not isinstance(owned, list) or not 1 <= len(owned) <= 16 or len(set(owned)) != len(owned):
        raise ValueError('Bounded exact optional ownership is required')
    for key in owned:
        kind, separator, name = key.partition('/')
        if not separator or identity(kind, name) != key:
            raise ValueError('Invalid owned identity')
    if (set(baselines) != BASELINE_PLATFORMS
            or set(request.get('policy', {})) != set(baselines)
            or set(candidates) != NATIVE_PLATFORMS):
        raise ValueError('Exact six baselines and three native candidates need explicit complete policy')
    for candidate in candidates.values():
        if set(entries(candidate)) != set(owned):
            raise ValueError('Native candidate inventory must exactly match admitted ownership')
    prefix = relative_path('packages/catalog/candidates/' + source + '/' + orchestration)
    objects = []
    reports = {}
    wrapper = {'schema': 1, 'artifact_class': 'optional-package-catalog', 'channel': 'staging',
               'source_sha': source, 'orchestration_source_sha': orchestration, 'stores': {}}
    total = 0
    for platform in sorted(baselines):
        relative_path(platform)
        manifest, report = compose_platform(platform, baselines[platform], candidates.get(platform), owned,
                                            request['policy'][platform], metadata.get(platform, {}))
        reports[platform] = report
        payload = json_bytes(manifest)
        key = prefix + '/stores/' + platform + '/packages-store-manifest.json'
        objects.append({'key': key, 'role': 'store-manifest', 'bytes': payload,
                        'sha256': hashlib.sha256(payload).hexdigest(), 'size_bytes': len(payload)})
        wrapper['stores'][platform] = {'manifest': 'stores/' + platform + '/packages-store-manifest.json'}
        for identity_key, entry in entries(manifest).items():
            total += entry['size_bytes']
            if total > MAX_TOTAL_BYTES or len(objects) >= MAX_ENTRIES:
                raise ValueError('Catalog payload capacity exceeded')
            objects.append({'key': prefix + '/stores/' + platform + '/' + entry['tarball'],
                            'role': 'archive', 'sha256': entry['sha256'], 'size_bytes': entry['size_bytes'],
                            'identity': identity_key, 'platform': platform})
    objects.sort(key=lambda item: (item['role'] != 'archive', item['key']))
    wrapper_bytes = json_bytes(wrapper)
    wrapper_key = prefix + '/release-manifest.json'
    objects.append({'key': wrapper_key, 'role': 'release-manifest', 'bytes': wrapper_bytes,
                    'sha256': hashlib.sha256(wrapper_bytes).hexdigest(), 'size_bytes': len(wrapper_bytes)})
    pointer = json_bytes({'schema': 1, 'artifact_class': 'optional-package-catalog',
                          'channel': 'staging', 'manifest': wrapper_key, 'source_sha': source,
                          'orchestration_source_sha': orchestration})
    return {'schema': 1, 'product_source_sha': source, 'orchestration_source_sha': orchestration,
            'prefix': prefix, 'objects': objects, 'pointer_key': 'packages/latest.json',
            'pointer_bytes': pointer, 'reports': reports}
