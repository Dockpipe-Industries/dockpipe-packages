"""Acquire exact official recipe inputs into a content-addressed task cache."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import tempfile
import time
import urllib.parse
import urllib.request

MAX_INPUT = 128 << 20
ALLOWED_HOSTS = {'go.dev', 'dl.google.com', 'github.com', 'release-assets.githubusercontent.com',
                 'archive.ubuntu.com', 'distfiles.ariadne.space', 'raw.githubusercontent.com', 'files.pythonhosted.org'}


def digest(path):
    checksum = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            checksum.update(block)
    return checksum.hexdigest()


def admit_url(url):
    parsed = urllib.parse.urlsplit(url)
    if (parsed.scheme != 'https' or parsed.hostname not in ALLOWED_HOSTS
            or parsed.username or parsed.password or parsed.port not in (None, 443)
            or parsed.query or parsed.fragment or '..' in urllib.parse.unquote(parsed.path).split('/')):
        raise ValueError('Tool acquisition requires an admitted official HTTPS host')


def admit_redirect(origin, destination):
    target = urllib.parse.urlsplit(destination)
    if not target.query:
        admit_url(destination)
        return

    # GitHub signs its release-asset transport URLs. Authored recipe URLs still
    # prohibit queries; only this exact official release-to-CDN edge admits one.
    admit_url(origin)
    source = urllib.parse.urlsplit(origin)
    query = urllib.parse.parse_qs(target.query, keep_blank_values=True)
    signed_keys = {'jwt', 'response-content-disposition', 'response-content-type',
                   'rscd', 'rsct', 'se', 'sig', 'ske', 'skoid', 'sks', 'skt',
                   'sktid', 'skv', 'sp', 'spr', 'sr', 'sv'}
    if (source.hostname != 'github.com'
            or not re.fullmatch(r'/[^/]+/[^/]+/releases/download/[^/]+/[^/]+', source.path)
            or target.hostname != 'release-assets.githubusercontent.com'
            or not re.fullmatch(r'/github-production-release-asset/[0-9]+/[0-9a-f-]{36}', target.path)
            or len(destination) > 16384 or not {'sig', 'se'} <= query.keys()
            or not query.keys() <= signed_keys
            or any(len(values) != 1 or not values[0] for values in query.values())):
        raise ValueError('Tool acquisition redirect is not an admitted signed release asset')
    admit_url(urllib.parse.urlunsplit(target._replace(query='')))


class OfficialRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file, code, message, headers, new_url):
        admit_redirect(request.full_url, new_url)
        return super().redirect_request(request, file, code, message, headers, new_url)


def acquire(entry, cache):
    admit_url(entry['url'])
    size = entry['size_bytes']
    checksum = entry['sha256']
    if type(size) is not int or not 0 < size <= MAX_INPUT:
        raise ValueError('Input size exceeds acquisition capacity')
    if not isinstance(checksum, str) or len(checksum) != 64 or any(character not in '0123456789abcdef' for character in checksum):
        raise ValueError('Input requires an exact SHA256')
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / checksum
    if target.exists():
        if target.is_symlink() or target.stat().st_size != size or digest(target) != checksum:
            raise ValueError('Existing cached input differs')
        return target
    deadline = time.monotonic() + 300
    opener = urllib.request.build_opener(OfficialRedirects())
    with tempfile.TemporaryDirectory(dir=cache, prefix='acquisition-') as temporary:
        candidate = Path(temporary) / 'input'
        with candidate.open('wb') as output:
            with opener.open(entry['url'], timeout=60) as response:
                remaining = size
                while remaining:
                    if time.monotonic() >= deadline:
                        raise ValueError('Toolchain acquisition exceeded its total deadline')
                    block = response.read(min(1 << 20, remaining))
                    if not block:
                        raise ValueError('Truncated toolchain acquisition')
                    output.write(block)
                    remaining -= len(block)
                if response.read(1):
                    raise ValueError('Toolchain acquisition exceeds declared size')
        # Close the writer before reopening for hashing or linking. Windows
        # does not permit reopening the default delete-on-close temporary file.
        if digest(candidate) != checksum:
            raise ValueError('Toolchain acquisition checksum mismatch')
        # No existing bytes are replaced; a new cache object is linked atomically.
        os.link(candidate, target)
    return target


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--recipe', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    arguments = parser.parse_args()
    entries = json.loads(arguments.recipe.read_text())
    for entry in entries:
        print(entry.get('id', entry.get('package')), acquire(entry, arguments.cache).name)
