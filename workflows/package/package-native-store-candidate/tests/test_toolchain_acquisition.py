"""Focused acquisition, resource and extraction rejection tests without network."""
import hashlib
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import test_native_store as fixtures
import toolchain_acquire as acquire
import toolchain_linux as prepare_linux


class ToolchainTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.payload = b'official-fixture'
        self.entry = {'url': 'https://go.dev/dl/fixture', 'size_bytes': len(self.payload),
                      'sha256': hashlib.sha256(self.payload).hexdigest()}

    def test_cache_exact_bytes_and_corruption(self):
        path = self.root / self.entry['sha256']
        path.write_bytes(self.payload)
        self.assertEqual(acquire.acquire(self.entry, self.root), path)
        path.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'cached input differs'):
            acquire.acquire(self.entry, self.root)

    def test_unapproved_origin_and_redirect_are_rejected(self):
        for url in ['http://go.dev/file', 'https://example.com/file', 'https://user@go.dev/file', 'https://go.dev/file?mutable=latest',
                    'https://go.dev/file#unbound']:
            with self.subTest(url=url), self.assertRaises(ValueError):
                acquire.admit_url(url)
        with self.assertRaises(ValueError):
            acquire.admit_redirect(self.entry['url'], 'https://example.com/file')

    def test_signed_github_transport_does_not_admit_authored_query_urls(self):
        origin = 'https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake.tar.gz'
        transport = ('https://release-assets.githubusercontent.com/'
                     'github-production-release-asset/537699/13316199-8d09-4e5a-9586-c4bbcb927726'
                     '?se=2026-10-24T01%3A00%3A00Z&sig=fixture&jwt=fixture')
        acquire.admit_redirect(origin, transport)
        request = acquire.urllib.request.Request(origin)
        redirected = acquire.OfficialRedirects().redirect_request(request, None, 302, '', {}, transport)
        self.assertEqual(redirected.full_url, transport)
        with self.assertRaises(ValueError):
            acquire.admit_url(transport)
        for source, target in [
            (self.entry['url'], transport),
            ('https://github.com/Kitware/CMake/blob/main/file', transport),
            (origin, transport.replace('release-assets.githubusercontent.com', 'example.com')),
            (origin, transport.replace('/github-production-release-asset/', '/other/')),
            (origin, transport + '&unexpected=value'),
            (origin, transport + '&sig=duplicate'),
            (origin, transport + '#fragment'),
            (origin, transport.replace('https://', 'http://')),
            (origin, transport.replace('sig=fixture', 'sig=')),
            (origin, transport + '&jwt=' + 'x' * 16384),
        ]:
            with self.subTest(source=source, target=target), self.assertRaises(ValueError):
                acquire.admit_redirect(source, target)

    def test_download_size_and_checksum_reject_before_cache_commit(self):
        for payload in [b'short', self.payload + b'long', b'x' * len(self.payload)]:
            opener = unittest.mock.Mock()
            opener.open.return_value = io.BytesIO(payload)
            with patch.object(acquire.urllib.request, 'build_opener', return_value=opener):
                with self.assertRaises(ValueError):
                    acquire.acquire(self.entry, self.root)
            self.assertEqual(list(self.root.iterdir()), [])

    def test_exact_download_commits_one_content_addressed_file(self):
        opener = unittest.mock.Mock()
        opener.open.return_value = io.BytesIO(self.payload)
        with patch.object(acquire.urllib.request, 'build_opener', return_value=opener):
            path = acquire.acquire(self.entry, self.root)
        self.assertEqual(path.read_bytes(), self.payload)
        self.assertEqual(path.name, self.entry['sha256'])

    def test_download_closes_writer_before_hash_and_link(self):
        opener = unittest.mock.Mock()
        opener.open.return_value = io.BytesIO(self.payload)
        original_open = Path.open
        original_digest = acquire.digest
        writer = None

        def tracked_open(path, mode='r', *args, **kwargs):
            nonlocal writer
            stream = original_open(path, mode, *args, **kwargs)
            if mode == 'wb':
                writer = stream
            return stream

        def closed_digest(path):
            self.assertIsNotNone(writer)
            self.assertTrue(writer.closed, 'Hashing must not reopen an active Windows writer')
            return original_digest(path)

        with patch.object(acquire.urllib.request, 'build_opener', return_value=opener), \
                patch.object(Path, 'open', tracked_open), \
                patch.object(acquire, 'digest', side_effect=closed_digest):
            path = acquire.acquire(self.entry, self.root)
        self.assertEqual(path.read_bytes(), self.payload)
        self.assertEqual(list(self.root.iterdir()), [path])

    def test_acquisition_bound_rejected_before_request(self):
        self.entry['size_bytes'] = acquire.MAX_INPUT + 1
        with patch.object(acquire.urllib.request, 'build_opener') as opener:
            with self.assertRaises(ValueError):
                acquire.acquire(self.entry, self.root)
            opener.assert_not_called()

    def test_archive_escape_and_link_are_rejected(self):
        for name in ['../outside', '/absolute', 'root/../../outside', 'root\\outside']:
            with self.subTest(name=name), self.assertRaises(ValueError):
                prepare_linux.relative_name(name)
        archive = self.root / 'input.tar.gz'
        with tarfile.open(archive, 'w:gz') as stream:
            member = tarfile.TarInfo('root/link')
            member.type = tarfile.SYMTYPE
            member.linkname = '/outside'
            stream.addfile(member)
        with patch.object(acquire, 'acquire', return_value=archive):
            with self.assertRaisesRegex(ValueError, 'Unsupported'):
                prepare_linux.extract({'format': 'tar.gz', 'root': 'root'}, self.root, self.root / 'output')

    def test_archive_root_and_member_count_bound(self):
        archive = self.root / 'input.tar.gz'
        with tarfile.open(archive, 'w:gz') as stream:
            for name in ['root/one', 'root/two']:
                member = tarfile.TarInfo(name)
                member.size = 1
                stream.addfile(member, io.BytesIO(b'x'))
        with patch.object(acquire, 'acquire', return_value=archive):
            with patch.object(prepare_linux, 'MAX_FILES', 1):
                with self.assertRaisesRegex(ValueError, 'capacity'):
                    prepare_linux.extract({'format': 'tar.gz', 'root': 'root'}, self.root, self.root / 'output')

    def test_existing_prefix_is_never_replaced(self):
        prefix = self.root / 'prefix'
        prefix.mkdir()
        original = prefix / 'user-state'
        original.write_bytes(b'preserve')
        with self.assertRaisesRegex(ValueError, 'fresh'):
            prepare_linux.prepare(self.root / 'missing.json', self.root, prefix)
        self.assertEqual(original.read_bytes(), b'preserve')

    def test_linux_recipe_has_exact_official_inputs_and_license_paths(self):
        root = Path(__file__).resolve().parents[4] / 'release/native-toolchain'
        entries = json.loads((root / 'linux-amd64.json').read_text())['inputs']
        self.assertEqual({entry['id'] for entry in entries}, {'go', 'cmake', 'ninja', 'pkgconf-source'})
        for entry in entries:
            acquire.admit_url(entry['url'])
            self.assertEqual(len(entry['sha256']), 64)
            self.assertTrue('license_path' in entry or 'license_asset' in entry)
        windows = json.loads((root / 'windows-amd64.json').read_text())
        self.assertEqual(windows['compiler']['id'], 'MSVC')
        self.assertEqual(windows['compiler']['version'], '19.51.36260.0')



if __name__ == '__main__':
    unittest.main()
