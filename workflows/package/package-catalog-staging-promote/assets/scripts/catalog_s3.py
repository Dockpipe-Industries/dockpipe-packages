"""Bounded AWS CLI adapter for the declared Cloudflare R2 staging bucket.

The selected CLI binary is hash-admitted by the request. Credential values stay
in the inherited process environment; arguments, receipts and errors never carry
credential values. There is no unconditional write or automatic retry path.
"""
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import tempfile
import time
from urllib.parse import urlsplit

from catalog_plan import relative_path
from catalog_transaction import PublicationStopped, StoredObject

MAX_OBJECT_BYTES = 512 * 1024 * 1024
MAX_COMMAND_OUTPUT = 65536


def file_digest(path, maximum):
    total = 0
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            total += len(block)
            if total > maximum:
                raise PublicationStopped('Object exceeds declared byte capacity')
            digest.update(block)
    return digest.hexdigest(), total


class S3Provider:
    def __init__(self, executable, executable_sha256, endpoint, bucket, temporary_root, immutable_prefix):
        self.executable = Path(executable)
        if (not self.executable.is_absolute() or self.executable.is_symlink()
                or not self.executable.is_file()
                or file_digest(self.executable, 256 * 1024 * 1024)[0] != executable_sha256):
            raise PublicationStopped('Object-store CLI identity admission failed')
        parsed = urlsplit(endpoint)
        if (parsed.scheme != 'https' or parsed.username or parsed.password or parsed.port
                or parsed.path not in {'', '/'} or parsed.query or parsed.fragment
                or not re.fullmatch(r'[0-9a-f]{32}\.r2\.cloudflarestorage\.com', parsed.hostname or '')):
            raise PublicationStopped('Endpoint must be the declared account R2 HTTPS endpoint')
        if not bucket or any(character not in 'abcdefghijklmnopqrstuvwxyz0123456789-.' for character in bucket):
            raise PublicationStopped('Bucket name is invalid')
        self.endpoint = endpoint.rstrip('/')
        self.bucket = bucket
        self.immutable_prefix = relative_path(immutable_prefix) + '/'
        if not re.fullmatch(r'packages/catalog/candidates/[0-9a-f]{40}/[0-9a-f]{40}/', self.immutable_prefix):
            raise PublicationStopped('Immutable prefix identity is invalid')
        self.temporary_root = Path(temporary_root)
        if not self.temporary_root.is_absolute() or os.name != 'posix':
            raise PublicationStopped('Provider requires a private POSIX temporary root')
        for ancestor in [self.temporary_root, *self.temporary_root.parents]:
            if ancestor.is_symlink():
                raise PublicationStopped('Temporary root has an aliased ancestor')
        state = self.temporary_root.stat()
        if (not self.temporary_root.is_dir() or state.st_uid != os.getuid()
                or state.st_mode & 0o777 != 0o700):
            raise PublicationStopped('Temporary root must be owned and mode 0700')
        status, output, _ = self.command(['s3api', 'put-object', '--generate-cli-skeleton', 'input'])
        try:
            model = json.loads(output)
        except (ValueError, UnicodeError) as error:
            raise PublicationStopped('CLI conditional-write model is unavailable') from error
        if status != 0 or not {'IfMatch', 'IfNoneMatch'} <= set(model):
            raise PublicationStopped('CLI does not expose required conditional PutObject headers')

    def command(self, arguments, watched_file=None, maximum_file_bytes=None):
        with tempfile.TemporaryDirectory(prefix='command-', dir=self.temporary_root) as directory:
            stdout = Path(directory) / 'stdout'
            stderr = Path(directory) / 'stderr'
            access_key = os.environ.get('AWS_ACCESS_KEY_ID', '')
            secret_key = os.environ.get('AWS_SECRET_ACCESS_KEY', '')
            if not access_key or not secret_key:
                raise PublicationStopped('Explicit staging credential references are not resolved')
            environment = {'PATH': '/usr/bin:/bin', 'HOME': str(self.temporary_root),
                           'AWS_ACCESS_KEY_ID': access_key, 'AWS_SECRET_ACCESS_KEY': secret_key,
                           'AWS_DEFAULT_REGION': 'auto', 'AWS_EC2_METADATA_DISABLED': 'true',
                           'AWS_CONFIG_FILE': os.devnull, 'AWS_SHARED_CREDENTIALS_FILE': os.devnull}
            environment['AWS_MAX_ATTEMPTS'] = '1'
            environment['AWS_RETRY_MODE'] = 'standard'
            environment['AWS_PAGER'] = ''
            with stdout.open('wb') as output, stderr.open('wb') as errors:
                process = subprocess.Popen(
                    [str(self.executable), '--endpoint-url', self.endpoint,
                     '--no-cli-pager', '--cli-connect-timeout', '10',
                     '--cli-read-timeout', '120', *arguments],
                    env=environment, stdin=subprocess.DEVNULL, stdout=output, stderr=errors)
                deadline = time.monotonic() + 125
                while process.poll() is None:
                    exceeded = (stdout.stat().st_size > MAX_COMMAND_OUTPUT
                                or stderr.stat().st_size > MAX_COMMAND_OUTPUT)
                    if watched_file is not None and watched_file.exists():
                        exceeded = exceeded or watched_file.stat().st_size > maximum_file_bytes
                    if exceeded or time.monotonic() >= deadline:
                        process.kill()
                        process.wait(timeout=5)
                        raise PublicationStopped('Operation capacity or deadline exceeded; readback required')
                    try:
                        process.wait(timeout=0.1)
                    except subprocess.TimeoutExpired:
                        pass
            if stdout.stat().st_size > MAX_COMMAND_OUTPUT or stderr.stat().st_size > MAX_COMMAND_OUTPUT:
                raise PublicationStopped('Object-store response exceeded metadata capacity')
            return process.returncode, stdout.read_bytes(), stderr.read_bytes()

    def read(self, key, maximum):
        relative_path(key)
        if key != 'packages/latest.json' and not key.startswith(self.immutable_prefix):
            raise PublicationStopped('Admin read key is outside publisher authority')
        if not 0 < maximum <= MAX_OBJECT_BYTES:
            raise PublicationStopped('Object read capacity is invalid')
        with tempfile.TemporaryDirectory(prefix='get-', dir=self.temporary_root) as directory:
            output_file = Path(directory) / 'object'
            status, output, errors = self.command([
                's3api', 'get-object', '--bucket', self.bucket, '--key', key, str(output_file)],
                watched_file=output_file, maximum_file_bytes=maximum)
            if status != 0:
                if b'An error occurred (NoSuchKey) when calling the GetObject operation:' in errors:
                    return None
                raise PublicationStopped('Admin GetObject failed; absence is not established')
            try:
                metadata = json.loads(output)
                etag = metadata['ETag']
            except (ValueError, KeyError, TypeError) as error:
                raise PublicationStopped('Admin object metadata is invalid') from error
            digest, size = file_digest(output_file, maximum)
            if not isinstance(etag, str) or not etag or len(etag) > 256 or metadata.get('ContentLength') != size:
                raise PublicationStopped('Admin object length or ETag is invalid')
            return StoredObject(digest, size, etag, metadata.get('CacheControl', ''))

    def create(self, key, source, expected):
        relative_path(key)
        if not key.startswith(self.immutable_prefix):
            raise PublicationStopped('Immutable key is outside publisher authority')
        self.put(key, source, expected, ['--if-none-match', '*'])

    def compare_and_swap(self, key, payload, etag):
        if key != 'packages/latest.json':
            raise PublicationStopped('Mutable key is outside publisher authority')
        condition = ['--if-none-match', '*'] if etag is None else ['--if-match', etag]
        self.put(key, payload, {'sha256': hashlib.sha256(payload).hexdigest(),
                                'size_bytes': len(payload)}, condition)

    def put(self, key, source, expected, condition):
        relative_path(key)
        if key == 'packages/latest.json':
            if (len(condition) != 2 or condition[0] not in {'--if-match', '--if-none-match'}
                    or (condition[0] == '--if-none-match' and condition[1] != '*')):
                raise PublicationStopped('Pointer mutation needs an explicit conditional header')
        elif not key.startswith(self.immutable_prefix) or condition != ['--if-none-match', '*']:
            raise PublicationStopped('Immutable mutation is outside authority or is unconditional')
        with tempfile.TemporaryDirectory(prefix='put-', dir=self.temporary_root) as directory:
            candidate = Path(directory) / 'payload'
            if isinstance(source, bytes):
                candidate.write_bytes(source)
            else:
                original = Path(source)
                if original.is_symlink() or not original.is_file():
                    raise PublicationStopped('Payload source is not an admitted regular file')
                copied = 0
                with original.open('rb') as input_stream, candidate.open('xb') as output_stream:
                    for block in iter(lambda: input_stream.read(1024 * 1024), b''):
                        copied += len(block)
                        if copied > expected['size_bytes']:
                            raise PublicationStopped('Payload changed or exceeded its admitted size')
                        output_stream.write(block)
                    output_stream.flush()
                    os.fsync(output_stream.fileno())
            digest, size = file_digest(candidate, MAX_OBJECT_BYTES)
            if digest != expected['sha256'] or size != expected['size_bytes']:
                raise PublicationStopped('Payload source differs from admitted immutable bytes')
            cache_control = 'no-cache' if key == 'packages/latest.json' else 'public,max-age=31536000,immutable'
            status, _, _ = self.command(['s3api', 'put-object', '--bucket', self.bucket,
                                         '--key', key, '--body', str(candidate), '--cache-control', cache_control, *condition])
            if status != 0:
                raise PublicationStopped('Conditional PutObject failed or outcome is unknown; readback required')
