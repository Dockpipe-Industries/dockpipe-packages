"""Exercise the catalog SDK's integrity, download and portable deadline boundaries."""
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock
import urllib.error

import fetch_sdk_release
import go_candidate


class SDKImportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.archive = self.root / 'sdk.tar.gz'
        self.metadata = self.root / 'sdk.json'
        self.destination = self.root / 'imported'

    def prepare(self, name='go.mod', payload=b'module dockpipe\n', member_payload=None):
        inventory = [{
            'path': name,
            'bytes': len(payload),
            'sha256': hashlib.sha256(payload).hexdigest(),
        }]
        with tarfile.open(self.archive, 'w:gz') as archive:
            member = tarfile.TarInfo('dockpipe-go-sdk/' + name)
            data = payload if member_payload is None else member_payload
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
        archive_data = self.archive.read_bytes()
        metadata = {
            'schema': 1, 'module': 'dockpipe', 'files': inventory,
            'source_digest': hashlib.sha256(json.dumps(inventory, separators=(',', ':')).encode()).hexdigest(),
            'bytes': len(archive_data), 'sha256': hashlib.sha256(archive_data).hexdigest(),
        }
        self.metadata.write_text(json.dumps(metadata))
        return hashlib.sha256(self.metadata.read_bytes()).hexdigest()

    def test_authenticated_artifact_import(self):
        digest = self.prepare()
        go_candidate.unpack_sdk(self.metadata, self.archive, digest, self.destination)
        self.assertEqual((self.destination / 'go.mod').read_bytes(), b'module dockpipe\n')

    def test_archive_traversal_rejected_before_import(self):
        digest = self.prepare(name='../outside')
        with self.assertRaisesRegex(ValueError, 'Unsafe SDK path'):
            go_candidate.unpack_sdk(self.metadata, self.archive, digest, self.destination)
        self.assertFalse(self.destination.exists())

    def test_member_digest_rejected_without_partial_destination(self):
        digest = self.prepare(payload=b'abcd', member_payload=b'zzzz')
        with self.assertRaisesRegex(ValueError, 'member checksum'):
            go_candidate.unpack_sdk(self.metadata, self.archive, digest, self.destination)
        self.assertFalse(self.destination.exists())

    def test_archive_digest_rejected(self):
        digest = self.prepare()
        self.archive.write_bytes(self.archive.read_bytes() + b'changed')
        with self.assertRaisesRegex(ValueError, 'archive checksum or size'):
            go_candidate.unpack_sdk(self.metadata, self.archive, digest, self.destination)

    def test_manifest_digest_rejected(self):
        self.prepare()
        with self.assertRaisesRegex(ValueError, 'independently supplied SHA-256'):
            go_candidate.unpack_sdk(self.metadata, self.archive, '0' * 64, self.destination)

    def test_json_lock_rejects_duplicates_and_capacity(self):
        lock = self.root / 'lock.json'
        lock.write_text('{"schema":1,"schema":2}')
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            go_candidate.read_json(lock, 100)
        with self.assertRaisesRegex(ValueError, 'exceeds'):
            go_candidate.read_json(lock, 3)


class Response(io.BytesIO):
    status = 200

    def __init__(self, data, headers=None):
        super().__init__(data)
        self.headers = headers or {}


class SDKDownloadTests(unittest.TestCase):
    def test_redirect_does_not_forward_token(self):
        location = 'https://release-assets.githubusercontent.com/asset'
        redirect = urllib.error.HTTPError('https://api.github.com/asset', 302, 'redirect', {'Location': location}, None)
        opener = mock.Mock()
        opener.open.side_effect = [redirect, Response(b'ok')]
        download = fetch_sdk_release.GitHubDownload('test-token', opener=opener)
        self.assertEqual(download.get('https://api.github.com/repos/example/sdk/releases/assets/1', 2, asset=True), b'ok')
        first, second = opener.open.call_args_list
        self.assertEqual(first.args[0].get_header('Authorization'), 'Bearer test-token')
        self.assertIsNone(second.args[0].get_header('Authorization'))

    def test_untrusted_redirect_rejected(self):
        with self.assertRaisesRegex(ValueError, 'allowed HTTPS asset host'):
            fetch_sdk_release.safe_asset_location('https://example.com/asset')

    def test_response_exceeds_capacity(self):
        opener = mock.Mock()
        opener.open.return_value = Response(b'toolarge')
        download = fetch_sdk_release.GitHubDownload('test-token', opener=opener)
        with self.assertRaisesRegex(ValueError, 'exceeds its capacity'):
            download.get('https://api.github.com/repos/example/sdk', 2)

    def test_total_deadline_exhausted_before_request(self):
        clock = mock.Mock(side_effect=[0, fetch_sdk_release.TOTAL_SECONDS + 1])
        opener = mock.Mock()
        download = fetch_sdk_release.GitHubDownload('test-token', opener=opener, clock=clock)
        with self.assertRaisesRegex(ValueError, 'total time limit'):
            download.get('https://api.github.com/repos/example/sdk', 2)
        opener.open.assert_not_called()

    def test_cli_uses_portable_worker_deadline(self):
        arguments = ['fetch_sdk_release.py', '--output', '/sdk-inputs']
        with mock.patch.object(fetch_sdk_release.sys, 'argv', arguments), \
                mock.patch.object(fetch_sdk_release.subprocess, 'run') as run:
            fetch_sdk_release.main()
        self.assertEqual(run.call_args.kwargs, {'check': True, 'timeout': fetch_sdk_release.TOTAL_SECONDS})
        self.assertEqual(run.call_args.args[0][-1], '--fetch-worker')

    def test_worker_timeout_is_sanitized(self):
        arguments = ['fetch_sdk_release.py', '--output', '/sdk-inputs']
        with mock.patch.object(fetch_sdk_release.sys, 'argv', arguments), \
                mock.patch.object(fetch_sdk_release.subprocess, 'run', side_effect=subprocess.TimeoutExpired('sensitive-url', 120)), \
                mock.patch('sys.stderr', new_callable=io.StringIO) as error:
            with self.assertRaises(SystemExit) as stopped:
                fetch_sdk_release.main()
        self.assertEqual(stopped.exception.code, 1)
        self.assertNotIn('sensitive-url', error.getvalue())
        self.assertIn('Locked SDK release fetch failed', error.getvalue())


if __name__ == '__main__':
    unittest.main()
