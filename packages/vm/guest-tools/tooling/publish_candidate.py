#!/usr/bin/env python3
"""Publish only an accepted dev push's exact attested Guest Tools prerelease."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import tempfile
import time
import urllib.parse
import urllib.request

import prepare_candidate as candidate


def run(arguments):
    with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as errors:
        result = subprocess.run(arguments, stdout=output, stderr=errors, timeout=180,
                                env=dict(os.environ, GH_HOST='github.com'))
        output.seek(0)
        errors.seek(0)
        data = output.read((2 << 20) + 1)
        diagnostic = errors.read(8193)
    if result.returncode != 0:
        status = re.search(rb'\bHTTP ([1-5][0-9]{2})\b', diagnostic)
        suffix = '; HTTP ' + status[1].decode() if status else ''
        raise ValueError('GitHub operation failed; no overwrite or delete will be attempted' + suffix)
    if len(data) > 2 << 20 or len(diagnostic) > 8192:
        raise ValueError('GitHub metadata exceeds its response bound')
    return data


def api(endpoint, method='GET', body=None):
    arguments = ['gh', 'api', '--hostname', 'github.com', endpoint, '--method', method]
    with tempfile.TemporaryDirectory(prefix='guest-release-request-') as temporary:
        if body is not None:
            path = Path(temporary) / 'request.json'
            path.write_text(json.dumps(body), encoding='utf-8')
            arguments.extend(['--input', str(path)])
        return json.loads(run(arguments), object_pairs_hook=candidate.unique_object)


def tag_target(tag):
    query = '''query($owner:String!,$name:String!,$ref:String!){
      repository(owner:$owner,name:$name){ref(qualifiedName:$ref){target{
        __typename oid ... on Tag {target{
          __typename oid ... on Tag {target{__typename oid}}
        }}
      }}}
    }'''
    result = json.loads(run(['gh', 'api', '--hostname', 'github.com', 'graphql',
                             '-f', 'query=' + query, '-f', 'owner=Dockpipe-Industries',
                             '-f', 'name=dockpipe-packages', '-f', 'ref=refs/tags/' + tag]))
    if result.get('errors'):
        raise ValueError('Cannot admit the candidate source tag')
    reference = result['data']['repository']['ref']
    if reference is None:
        return None
    target = reference['target']
    for _ in range(3):
        if target.get('__typename') == 'Commit' and candidate.source_value(target.get('oid')):
            return target['oid']
        if target.get('__typename') != 'Tag':
            break
        target = target['target']
    raise ValueError('Candidate tag does not resolve to a bounded exact commit')


def find_release(tag):
    matches = []
    for page in range(1, 21):
        releases = api('repos/' + candidate.REPOSITORY + '/releases?per_page=100&page=' + str(page))
        if not isinstance(releases, list) or len(releases) > 100:
            raise ValueError('Release inventory is malformed')
        if any(not isinstance(release, dict) for release in releases):
            raise ValueError('Release inventory entries are malformed')
        matches.extend(release for release in releases if release.get('tag_name') == tag)
        if len(releases) < 100:
            if len(matches) > 1:
                raise ValueError('Candidate release identity is ambiguous')
            return matches[0] if matches else None
    raise ValueError('Release inventory exceeds its bounded discovery capacity')


def local_assets(directory, source_sha):
    if not directory.is_absolute() or directory.is_symlink():
        raise ValueError('Select explicit regular prepared assets')
    manifest, manifest_bytes = candidate.document(directory / candidate.MANIFEST_NAME)
    # Reconstruct the manifest from independently checked native inputs. Never
    # trust a caller-edited publication plan or a manifest's own hash assertions.
    with tempfile.TemporaryDirectory(prefix='guest-release-admission-') as temporary:
        inputs = Path(temporary)
        data = {}
        for role, name in candidate.NAMES.items():
            value = candidate.regular_bytes(directory / name, 256 << 20 if role == 'guest_tools' else 65536)
            (inputs / name).write_bytes(value)
            data[name] = value
        if candidate.manifest_for(inputs, source_sha) != manifest:
            raise ValueError('Prepared manifest differs from actual native input bytes')
    data[candidate.MANIFEST_NAME] = manifest_bytes
    if {path.name for path in directory.iterdir()} != set(data):
        raise ValueError('Prepared asset directory has missing or unlisted files')
    assets = {name: {'size_bytes': len(value), 'sha256': candidate.digest(value)}
              for name, value in data.items()}
    return manifest, assets, data


def missing_draft_assets(release, target, manifest, assets):
    if (target is not None and target != manifest['source_sha']
            or release.get('tag_name') != manifest['tag']
            or release.get('target_commitish') != manifest['source_sha']
            or release.get('prerelease') is not True or type(release.get('draft')) is not bool):
        raise ValueError('Existing candidate identity conflicts; no mutation permitted')
    if release['draft'] is False:
        candidate.validate_remote_release(release, target, manifest, assets)
        return []
    remote = release.get('assets')
    if not isinstance(remote, list) or len(remote) > len(assets):
        raise ValueError('Draft assets exceed the exact candidate set')
    seen = set()
    for item in remote:
        name = item.get('name')
        if name not in assets or name in seen:
            raise ValueError('Draft contains unexpected or duplicate assets')
        seen.add(name)
        if (type(item.get('size')) is not int or item['size'] != assets[name]['size_bytes']
                or item.get('digest') != 'sha256:' + assets[name]['sha256']):
            raise ValueError('Draft asset bytes conflict; never clobber or delete')
    return sorted(set(assets) - seen)


def latest_release():
    try:
        release = api('repos/' + candidate.REPOSITORY + '/releases/latest')
    except ValueError as error:
        if str(error).endswith('; HTTP 404'):
            return None
        raise
    if (not isinstance(release, dict) or type(release.get('id')) is not int
            or release['id'] <= 0 or not isinstance(release.get('tag_name'), str)):
        raise ValueError('Latest release identity is malformed')
    return release['id'], release['tag_name']


def verify_attestations(data, source_sha):
    with tempfile.TemporaryDirectory(prefix='guest-attestation-admission-') as temporary:
        for name, payload in data.items():
            path = Path(temporary) / name
            path.write_bytes(payload)
            run(['gh', 'attestation', 'verify', str(path), '--repo', candidate.REPOSITORY,
                 '--signer-workflow', candidate.REPOSITORY + '/.github/workflows/guest-tools-candidate.yml',
                 '--source-digest', source_sha, '--source-ref', 'refs/heads/dev',
                 '--deny-self-hosted-runners'])


class PublicRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file_pointer, code, message, headers, new_url):
        validate_public_url(new_url)
        return super().redirect_request(request, file_pointer, code, message, headers, new_url)


def validate_public_url(url):
    parsed = urllib.parse.urlsplit(url)
    if (parsed.scheme != 'https' or parsed.username or parsed.password
            or parsed.port not in (None, 443) or parsed.hostname not in {
                'github.com', 'release-assets.githubusercontent.com', 'objects.githubusercontent.com'}):
        raise ValueError('Public readback must remain on approved anonymous HTTPS asset hosts')


def open_public(url):
    validate_public_url(url)
    return urllib.request.build_opener(PublicRedirect()).open(url, timeout=60)


def public_readback(manifest, assets):
    prefix = 'https://github.com/' + candidate.REPOSITORY + '/releases/download/' + manifest['tag'] + '/'
    started = time.monotonic()
    for name, expected in assets.items():
        checksum = hashlib.sha256()
        remaining = expected['size_bytes']
        # Public anonymous transport does not forward GH_TOKEN or API credentials.
        with open_public(prefix + name) as response:
            while remaining:
                if time.monotonic() - started > 300:
                    raise ValueError('Public candidate readback deadline exceeded')
                chunk = response.read(min(1 << 20, remaining))
                if not chunk:
                    raise ValueError('Public candidate readback is truncated')
                checksum.update(chunk)
                remaining -= len(chunk)
            if response.read(1):
                raise ValueError('Public candidate readback exceeds its pinned size')
        if checksum.hexdigest() != expected['sha256']:
            raise ValueError('Public candidate readback differs from qualified bytes')


def publish(directory):
    source_sha = os.environ.get('GITHUB_SHA', '')
    if (os.environ.get('GITHUB_EVENT_NAME') != 'push' or os.environ.get('GITHUB_REF') != 'refs/heads/dev'
            or os.environ.get('GITHUB_REPOSITORY') != candidate.REPOSITORY
            or not candidate.source_value(source_sha)):
        raise ValueError('Publication requires an accepted exact owning dev push')
    manifest, assets, data = local_assets(directory, source_sha)
    verify_attestations(data, source_sha)
    latest = latest_release()
    tag = manifest['tag']
    release = find_release(tag)
    target = tag_target(tag)
    if target is not None and target != source_sha:
        raise ValueError('Candidate tag already identifies different source')
    if release is None:
        release = api('repos/' + candidate.REPOSITORY + '/releases', 'POST', {
            'tag_name': tag, 'target_commitish': source_sha, 'name': 'Guest Tools immutable candidate',
            'body': 'Native Windows Guest Tools only. No production channel update.',
            'draft': True, 'prerelease': True, 'make_latest': 'false'})
    missing = missing_draft_assets(release, target, manifest, assets)
    with tempfile.TemporaryDirectory(prefix='guest-release-upload-') as temporary:
        # Upload the admitted immutable byte snapshot, not mutable source files.
        for name in missing:
            path = Path(temporary) / name
            path.write_bytes(data[name])
            run(['gh', 'release', 'upload', tag, str(path), '--repo', 'https://github.com/' + candidate.REPOSITORY])
    endpoint = 'repos/' + candidate.REPOSITORY + '/releases/' + str(release['id'])
    release = api(endpoint)
    candidate.validate_remote_release(release, tag_target(tag), manifest, assets)
    if release['draft']:
        release = api(endpoint, 'PATCH', {'draft': False, 'prerelease': True, 'make_latest': 'false'})
    final = api(endpoint)
    candidate.validate_remote_release(final, tag_target(tag), manifest, assets)
    if final['draft'] is not False:
        raise ValueError('Candidate remains a draft')
    public_readback(manifest, assets)
    if latest_release() != latest:
        raise ValueError('Latest release changed during candidate publication; no rollback attempted')
    return {'schema': 1, 'tag': tag, 'source_sha': source_sha,
            'public_readback': 'passed', 'attestations_verified': True, 'latest_unchanged': True, 'assets': assets}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    arguments = parser.parse_args()
    if not arguments.receipt.is_absolute() or arguments.receipt.exists():
        raise ValueError('Select a fresh explicit publication receipt')
    result = publish(arguments.assets)
    arguments.receipt.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
