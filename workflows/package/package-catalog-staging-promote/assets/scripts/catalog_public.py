"""Anonymous bounded same-origin curl reads; only HTTP404 establishes absence.

Curl is selected explicitly because this staging origin has returned HTTP403 to
Python's HTTP transport while the established curl transport returned valid bytes.
Redirect destinations are validated before a second request, not after download.
"""
import email.parser
import os
from pathlib import Path
import subprocess
import shutil
import tempfile
import time
import urllib.parse

from catalog_plan import relative_path
from catalog_s3 import file_digest
from catalog_transaction import PublicationStopped, StoredObject

ORIGIN = 'https://packages.staging.dockpipe.com'
MAX_BYTES = 512 * 1024 * 1024


def redirect_target(current, location):
    target = urllib.parse.urljoin(current, location)
    parsed = urllib.parse.urlsplit(target)
    if (parsed.scheme != 'https' or parsed.netloc != 'packages.staging.dockpipe.com'
            or parsed.username or parsed.password or parsed.query or parsed.fragment):
        raise PublicationStopped('Public catalog redirect left the admitted origin')
    relative_path(parsed.path.lstrip('/'))
    return target


class PublicReader:
    def __init__(self, executable, executable_sha256, temporary_root):
        self.executable = Path(executable)
        if (not self.executable.is_absolute() or self.executable.is_symlink()
                or file_digest(self.executable, 64 * 1024 * 1024)[0] != executable_sha256):
            raise PublicationStopped('Public-read curl identity differs')
        self.temporary_root = Path(temporary_root)
        if (not self.temporary_root.is_absolute() or not self.temporary_root.is_dir()
                or os.name != 'posix'):
            raise PublicationStopped('Public-read private POSIX root is unavailable')
        for path in [self.temporary_root, *self.temporary_root.parents]:
            if path.is_symlink():
                raise PublicationStopped('Public-read temporary root has an alias')
        state = self.temporary_root.stat()
        if state.st_uid != os.getuid() or state.st_mode & 0o777 != 0o700:
            raise PublicationStopped('Public-read temporary root must be owned0700')

    def __call__(self, key, maximum):
        return self.fetch(key, maximum)

    def fetch(self, key, maximum, destination=None):
        relative_path(key)
        if not 0 < maximum <= MAX_BYTES:
            raise PublicationStopped('Public read byte capacity is invalid')
        target = ORIGIN + '/' + key
        deadline = time.monotonic() + 125
        with tempfile.TemporaryDirectory(prefix='public-', dir=self.temporary_root) as directory:
            root = Path(directory)
            body = root / 'body'
            headers = root / 'headers'
            output = root / 'stdout'
            errors = root / 'stderr'
            for redirect in range(4):
                remaining = int(deadline - time.monotonic())
                if remaining <= 0:
                    raise PublicationStopped('Public read deadline exceeded')
                arguments = [str(self.executable), '--disable', '--silent', '--show-error',
                             '--proto', '=https', '--max-redirs', '0', '--connect-timeout', '10',
                             '--max-time', str(remaining), '--max-filesize', str(max(maximum, 65536)),
                             '--header', 'Accept-Encoding: identity', '--output', str(body),
                             '--dump-header', str(headers), '--write-out', '%{http_code}', target]
                environment = {'PATH': '/usr/bin:/bin', 'HOME': str(root)}
                with output.open('wb') as stdout, errors.open('wb') as stderr:
                    process = subprocess.Popen(arguments, env=environment, stdin=subprocess.DEVNULL,
                                               stdout=stdout, stderr=stderr)
                    while process.poll() is None:
                        exceeded = any(path.exists() and path.stat().st_size > limit for path, limit in [
                            (body, max(maximum, 65536)), (headers, 65536), (output, 4096), (errors, 65536)])
                        if exceeded or time.monotonic() >= deadline:
                            process.kill()
                            process.wait(timeout=5)
                            raise PublicationStopped('Public transport exceeded deadline or byte capacity')
                        try:
                            process.wait(timeout=0.1)
                        except subprocess.TimeoutExpired:
                            pass
                if (process.returncode != 0 or output.stat().st_size > 4096
                        or errors.stat().st_size > 65536 or headers.stat().st_size > 65536):
                    raise PublicationStopped('Public transport failed; absence is not established')
                status = output.read_text().strip()
                raw_headers = headers.read_bytes()
                lines = raw_headers.split(b'\n', 1)
                if len(lines) != 2 or not lines[0].startswith(b'HTTP/'):
                    raise PublicationStopped('Public response headers are invalid')
                fields = email.parser.BytesParser().parsebytes(lines[1])
                if status == '404':
                    return None
                if status in {'301', '302', '303', '307', '308'}:
                    if redirect == 3 or not fields.get('Location'):
                        raise PublicationStopped('Public redirect capacity or metadata is invalid')
                    target = redirect_target(target, fields['Location'])
                    continue
                if status != '200' or fields.get('Content-Encoding', 'identity') != 'identity':
                    raise PublicationStopped('Public HTTP status or encoding is invalid')
                digest, size = file_digest(body, maximum)
                if destination is not None:
                    with Path(destination).open('xb') as target, body.open('rb') as source:
                        shutil.copyfileobj(source, target, 1024 * 1024)
                        target.flush()
                        os.fsync(target.fileno())
                return StoredObject(digest, size, fields.get('ETag', ''))
        raise PublicationStopped('Public read did not establish a response')
