"""Shared immutable catalog release transaction; callers admit qualification first."""
import hashlib
import json

REPOSITORY = 'Dockpipe-Industries/dockpipe-packages'

def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def verify_reference(reference, source, missing=False):
    if reference is None and missing:
        return
    target = reference.get('target') if isinstance(reference, dict) else None
    for _ in range(3):
        if not isinstance(target, dict):
            break
        if target.get('__typename') == 'Commit' and target.get('oid') == source:
            return
        if target.get('__typename') != 'Tag':
            break
        target = target.get('target')
    raise ValueError('Candidate reference does not resolve to the exact source commit')


def inventory(release, tag, source, assets, complete):
    if (release.get('tag_name') != tag or release.get('target_commitish') != source
            or release.get('prerelease') is not True or type(release.get('draft')) is not bool
            or type(release.get('id')) is not int or release['id'] <= 0):
        raise ValueError('Existing release source/class identity differs')
    remote = release.get('assets')
    if not isinstance(remote, list) or len(remote) > len(assets):
        raise ValueError('Remote candidate has unexpected assets')
    seen = set()
    for item in remote:
        if not isinstance(item, dict):
            raise ValueError('Remote asset metadata is invalid')
        name = item.get('name')
        if name not in assets or name in seen:
            raise ValueError('Remote candidate has extra or duplicate asset names')
        seen.add(name)
        expected = assets[name]
        if (item.get('size') != expected['size'] or item.get('digest') != 'sha256:' + expected['sha256']
                or item.get('state') != 'uploaded'):
            raise ValueError('Existing asset differs; candidate bytes are never replaced')
    if complete and seen != set(assets):
        raise ValueError('Published or completed candidate inventory is incomplete')
    return seen


def publish_assets(release_document, by_platform, output, metadata_name, client):
    metadata = (json.dumps(release_document, sort_keys=True, indent=2) + '\n').encode()
    if output.exists():
        raise ValueError('Publication output must be fresh; immutable inputs are never rewritten')
    output.mkdir(parents=True)
    metadata_path = output / metadata_name
    metadata_path.write_bytes(metadata)
    assets = {path.name: {'path': path, 'size': path.stat().st_size, 'sha256': sha256(path)}
              for path in by_platform.values()}
    assets[metadata_name] = {'path': metadata_path, 'size': len(metadata), 'sha256': hashlib.sha256(metadata).hexdigest()}
    tag = release_document['tag']
    source = release_document['source_commit']
    latest = client.latest()
    verify_reference(client.reference('refs/heads/dev'), source)
    release = client.find(tag)
    verify_reference(client.reference('refs/tags/' + tag), source,
                     missing=release is None or release.get('draft') is True)
    if release is None:
        client.create(tag, source)
        release = client.find(tag)
        if release is None:
            raise ValueError('Created draft is unavailable; inspect and explicitly resume later')
    seen = inventory(release, tag, source, assets, complete=not release['draft'])
    if release['draft']:
        for name in sorted(set(assets) - seen):
            client.upload(tag, assets[name]['path'])
            release = client.release(release['id'])
            inventory(release, tag, source, assets, complete=False)
        inventory(release, tag, source, assets, complete=True)
        verify_reference(client.reference('refs/heads/dev'), source)
        verify_reference(client.reference('refs/tags/' + tag), source, missing=True)
        client.publish(tag)
    final = client.release(release['id'])
    inventory(final, tag, source, assets, complete=True)
    if final['draft'] is not False:
        raise ValueError('Candidate remains a draft')
    verify_reference(client.reference('refs/tags/' + tag), source)
    prefix = 'https://github.com/' + REPOSITORY + '/releases/download/' + tag + '/'
    for name, asset in assets.items():
        if client.public_digest(prefix + name, asset['size']) != asset['sha256']:
            raise ValueError('Unauthenticated public asset readback differs from qualified candidate')
    if client.latest() != latest:
        raise ValueError('Latest/production release changed during candidate operation')
    receipt = dict(release_document, success=True, release_id=final['id'],
                   public_asset_readback=True, latest_unchanged=True, published=True)
    (output / 'publication-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt
