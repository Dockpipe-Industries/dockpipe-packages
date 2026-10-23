#!/usr/bin/env python3
"""Fetch the locked public Dockpipe SDK without forwarding credentials to assets."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

import go_candidate


MAX_METADATA_BYTES = 1 << 20
REQUEST_TIMEOUT = 15
TOTAL_SECONDS = 120
MAX_REDIRECTS = 3
ASSET_HOSTS = {'release-assets.githubusercontent.com', 'objects.githubusercontent.com'}


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, url):
        return None


def load_source(root):
    source = go_candidate.read_json(root / 'release/sdk-source.lock.json', go_candidate.MAX_LOCK_BYTES)
    required = {'schema', 'repository', 'tag', 'manifest_asset', 'archive_asset'}
    if not isinstance(source, dict) or set(source) != required or type(source['schema']) is not int or source['schema'] != 1:
        raise ValueError('Unsupported SDK release source descriptor')
    if not all(isinstance(source[field], str) for field in required - {'schema'}):
        raise ValueError('SDK release source fields must be strings')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9-]*/[A-Za-z0-9_][A-Za-z0-9_.-]*', source['repository']):
        raise ValueError('SDK release repository must be an exact GitHub owner/repository')
    lock = go_candidate.read_json(root / 'release/dependencies.lock.json', go_candidate.MAX_LOCK_BYTES)['dockpipe_go_sdk']
    identity = {
        'sdk_version': lock['version'], 'source_commit': lock['source_commit'],
        'source_digest': lock['source_digest'], 'sha256': lock['archive_sha256'],
    }
    go_candidate.validate_sdk_pin(root, identity, lock['manifest_sha256'])
    stem = f"dockpipe-go-sdk-{lock['source_digest'][:16]}"
    if source['tag'] != f"dockpipe-go-sdk-{lock['version']}" or source['manifest_asset'] != stem + '.json' or source['archive_asset'] != stem + '.tar.gz':
        raise ValueError('SDK release source must identify the committed SDK snapshot')
    return source, lock


def safe_asset_location(location):
    parsed = urllib.parse.urlsplit(location)
    if parsed.scheme != 'https' or parsed.hostname not in ASSET_HOSTS or parsed.username or parsed.password or parsed.fragment or parsed.port not in {None, 443}:
        raise ValueError('SDK asset redirect target is not an allowed HTTPS asset host')


class GitHubDownload:
    def __init__(self, token, opener=None, clock=time.monotonic):
        if not token or any(ord(character) < 33 or ord(character) > 126 for character in token):
            raise ValueError('GH_TOKEN is required for locked SDK release metadata')
        self.token = token
        self.opener = opener or urllib.request.build_opener(NoRedirect())
        self.clock = clock
        self.deadline = clock() + TOTAL_SECONDS

    def remaining(self):
        remaining = self.deadline - self.clock()
        if remaining <= 0:
            raise ValueError('SDK release download exceeded its total time limit')
        return min(REQUEST_TIMEOUT, remaining)

    def get(self, url, maximum, asset=False):
        parsed = urllib.parse.urlsplit(url)
        if parsed.scheme != 'https' or parsed.hostname != 'api.github.com' or parsed.username or parsed.password or parsed.fragment or parsed.port not in {None, 443}:
            raise ValueError('SDK API requests must target HTTPS api.github.com')
        for redirect in range(MAX_REDIRECTS + 1):
            headers = {'Accept': 'application/octet-stream' if asset else 'application/vnd.github+json',
                       'User-Agent': 'dockpipe-catalog-locked-sdk', 'X-GitHub-Api-Version': '2022-11-28'}
            # Only the initial, constructed api.github.com request receives the token.
            if redirect == 0:
                headers['Authorization'] = f'Bearer {self.token}'
            request = urllib.request.Request(url, headers=headers)
            try:
                response = self.opener.open(request, timeout=self.remaining())
            except urllib.error.HTTPError as error:
                if not asset or error.code not in {301, 302, 303, 307, 308}:
                    code = error.code
                    error.close()
                    raise ValueError(f'GitHub SDK request failed with HTTP {code}') from None
                location = error.headers.get('Location', '')
                error.close()
                safe_asset_location(location)
                url = location
                continue
            with response:
                if response.status != 200:
                    raise ValueError('GitHub SDK request did not return HTTP 200')
                length = response.headers.get('Content-Length')
                if length is not None and (not length.isdecimal() or int(length) > maximum):
                    raise ValueError('SDK response length exceeds its capacity')
                chunks = []
                received = 0
                while True:
                    self.remaining()
                    chunk = response.read1(min(65536, maximum + 1 - received))
                    if not chunk:
                        break
                    received += len(chunk)
                    if received > maximum:
                        raise ValueError('SDK response exceeds its capacity')
                    chunks.append(chunk)
                if length is not None and received != int(length):
                    raise ValueError('SDK response was truncated')
                self.remaining()
                return b''.join(chunks)
        raise ValueError('SDK asset exceeded its redirect limit')


def select_asset(release, name, maximum):
    matches = [asset for asset in release['assets'] if isinstance(asset, dict) and asset.get('name') == name]
    if len(matches) != 1:
        raise ValueError('Locked SDK release asset is missing or ambiguous')
    asset = matches[0]
    if type(asset.get('id')) is not int or asset['id'] <= 0 or type(asset.get('size')) is not int or not 0 < asset['size'] <= maximum or asset.get('state') != 'uploaded':
        raise ValueError('Locked SDK asset metadata is invalid or exceeds capacity')
    return asset


def fetch(root, output, token, downloader=None):
    source, lock = load_source(root)
    if output.exists() or output.is_symlink():
        raise ValueError('Use a fresh SDK download output directory')
    client = downloader or GitHubDownload(token)
    api = f"https://api.github.com/repos/{source['repository']}"
    tag = urllib.parse.quote(source['tag'], safe='')
    release = json.loads(client.get(f'{api}/releases/tags/{tag}', MAX_METADATA_BYTES), object_pairs_hook=go_candidate.unique_lock_keys)
    if not isinstance(release, dict) or release.get('tag_name') != source['tag'] or release.get('draft') is not False or not isinstance(release.get('assets'), list):
        raise ValueError('GitHub release metadata differs from the locked SDK source')
    manifest_asset = select_asset(release, source['manifest_asset'], go_candidate.MAX_MANIFEST_BYTES)
    archive_asset = select_asset(release, source['archive_asset'], go_candidate.MAX_ARCHIVE_BYTES)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='sdk-release-', dir=output.parent) as temporary:
        staging = Path(temporary)
        inputs = staging / 'inputs'
        inputs.mkdir()
        for name, asset, maximum, digest in [
            ('sdk.json', manifest_asset, go_candidate.MAX_MANIFEST_BYTES, lock['manifest_sha256']),
            ('sdk.tar.gz', archive_asset, go_candidate.MAX_ARCHIVE_BYTES, lock['archive_sha256']),
        ]:
            payload = client.get(f"{api}/releases/assets/{asset['id']}", maximum, asset=True)
            if len(payload) != asset['size'] or hashlib.sha256(payload).hexdigest() != digest:
                raise ValueError('Downloaded SDK asset does not match its committed size or digest')
            (inputs / name).write_bytes(payload)
            if name == 'sdk.json':
                manifest = json.loads(payload, object_pairs_hook=go_candidate.unique_lock_keys)
                go_candidate.validate_sdk_pin(root, manifest, digest)
        go_candidate.unpack_sdk(inputs / 'sdk.json', inputs / 'sdk.tar.gz', lock['manifest_sha256'], staging / 'verified-source')
        # Publish only pinned input assets; extracted source remains importer-owned.
        inputs.rename(output)



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--fetch-worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        if args.fetch_worker:
            fetch(Path(__file__).resolve().parents[1], args.output, os.environ.get('GH_TOKEN', ''))
        else:
            # A subprocess deadline bounds response headers and bodies on Unix
            # and Windows, including peers that continually avoid socket timeouts.
            subprocess.run(
                [sys.executable, str(Path(__file__).resolve()), '--output',
                 str(args.output), '--fetch-worker'],
                check=True, timeout=TOTAL_SECONDS,
            )
    except Exception:
        # HTTP/TLS/URL errors can contain signed URLs or credential-bearing context.
        parser.exit(1, 'Locked SDK release fetch failed; check source pins, release assets and GH_TOKEN read access.\n')


if __name__ == '__main__':
    main()
