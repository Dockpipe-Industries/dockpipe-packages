import copy
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock
import urllib.request

SCRIPTS = Path(__file__).resolve().parents[1] / 'assets/scripts'
sys.path.insert(0, str(SCRIPTS))
import macho_metadata
import native_inputs_macos as native

ROOT = Path(__file__).resolve().parents[4] / 'release/native-inputs'


@unittest.skipIf(os.name == 'nt', 'macOS recipe contract uses POSIX ownership and aliases')
class MacInputTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.profile = json.loads((ROOT / 'darwin-arm64.json').read_text())

    def test_profile_exact_public_inputs_and_baseline(self):
        self.assertIs(native.validate(self.profile), self.profile)
        self.assertEqual(len(self.profile['archives']), 9)
        self.assertEqual(len(self.profile['libraries']), 6)
        for change in ('compiler', 'platform', 'missing', 'url', 'boolean-size'):
            profile = copy.deepcopy(self.profile)
            if change == 'compiler':
                profile['compiler']['version'] = '21.0.0.21000334'
            elif change == 'platform':
                profile['platform'] = 'linux-arm64'
            elif change == 'missing':
                profile['archives'].pop()
            elif change == 'url':
                profile['archives'][2]['asset']['url'] = 'https://ghcr.io/v2/private/repository/blobs/sha256:' + 'a' * 64
            else:
                profile['archives'][2]['asset']['size_bytes'] = True
            with self.subTest(change=change), self.assertRaises(ValueError):
                native.validate(profile)

    def entry(self):
        digest = hashlib.sha256(b'abc').hexdigest()
        return {'package': 'glib', 'asset': {'url': 'https://ghcr.io/v2/homebrew/core/glib/blobs/sha256:' + digest,
                'sha256': digest, 'size_bytes': 3, 'format': 'tar.gz', 'root': ''}}

    def test_oci_anonymous_pull_only_scope_and_exact_hash(self):
        seen = []
        class Opener:
            def open(self, request, timeout):
                seen.append(request)
                if isinstance(request, str):
                    return io.BytesIO(b'{"token":"public-anonymous-fixture"}')
                self_request = request
                self.assertion = self_request.get_header('Authorization')
                return io.BytesIO(b'abc')
        opener = Opener()
        with mock.patch.dict(os.environ, {'GH_TOKEN': 'private-must-not-be-used'}):
            result = native.acquire_oci(self.entry(), self.root / 'cache', True, opener)
        self.assertEqual(result.read_bytes(), b'abc')
        self.assertIn('repository%3Ahomebrew%2Fcore%2Fglib%3Apull', seen[0])
        self.assertEqual(opener.assertion, 'Bearer public-anonymous-fixture')
        self.assertEqual(len(list((self.root / 'cache').iterdir())), 1)
        self.assertNotIn(b'private-must-not-be-used', result.read_bytes())

    def test_oci_missing_wrong_bytes_bad_identity_and_cache_symlink_fail(self):
        with self.assertRaises(ValueError):
            native.acquire_oci(self.entry(), self.root / 'cache')
        bad = self.entry()
        bad['asset']['sha256'] = '../escape'
        with self.assertRaises(ValueError):
            native.acquire_oci(bad, self.root / 'cache')
        entry = self.entry()
        (self.root / 'cache' / entry['asset']['sha256']).symlink_to(self.root / 'missing')
        with self.assertRaises(ValueError):
            native.acquire_oci(entry, self.root / 'cache')

    def test_oci_redirect_drops_bearer_and_refuses_downgrade(self):
        request = urllib.request.Request('https://ghcr.io/blob', headers={'Authorization': 'Bearer public-fixture'})
        redirected = native.AnonymousOCIRedirect().redirect_request(request, None, 302, '', {},
                        'https://pkg-containers.githubusercontent.com/blob')
        self.assertIsNone(redirected.get_header('Authorization'))
        for url in ('http://ghcr.io/blob', 'https://example.invalid/blob'):
            with self.subTest(url=url), self.assertRaises(ValueError):
                native.AnonymousOCIRedirect().redirect_request(request, None, 302, '', {}, url)

    def test_declared_bottle_references_have_no_ambient_brew_fallback(self):
        cellar = self.root / 'Cellar'
        library = cellar / 'glib/2.90.1/lib/libglib.dylib'
        library.parent.mkdir(parents=True)
        library.write_bytes(b'fixture')
        versions = {'glib': '2.90.1'}
        expected = library.resolve()
        for reference in ('@@HOMEBREW_PREFIX@@/opt/glib/lib/libglib.dylib',
                          '@@HOMEBREW_CELLAR@@/glib/2.90.1/lib/libglib.dylib',
                          '/opt/homebrew/Cellar/glib/2.90.1/lib/libglib.dylib'):
            self.assertEqual(native.target_path(reference, cellar, versions, library), expected)
        for reference in ('/usr/local/lib/other.dylib', '@rpath/unknown.dylib',
                          '/opt/homebrew/opt/missing/lib/libglib.dylib',
                          '@@HOMEBREW_CELLAR@@/../../outside'):
            with self.subTest(reference=reference), self.assertRaises((ValueError, FileNotFoundError)):
                native.target_path(reference, cellar, versions, library)

    def test_wrong_native_builder_is_rejected_before_acquisition_or_output(self):
        with mock.patch.object(native.platform, 'system', return_value='Darwin'), \
                mock.patch.object(native.platform, 'machine', return_value='arm64'), \
                mock.patch.object(native.platform, 'mac_ver', return_value=('27.0.1', (), '')):
            acquire = mock.Mock()
            with self.assertRaisesRegex(ValueError, 'macos-15'):
                native.prepare(self.profile, self.root / 'out', self.root / 'tools', self.root / 'cache', False, acquire, {})
            acquire.assert_not_called()
        self.assertFalse((self.root / 'out').exists())



    def test_macho_wrong_cpu_high_minimum_and_truncated_commands_refused(self):
        def binary(cpu=0x0100000C, minimum=0xF0000, span=24):
            command = struct.pack('<IIIIII', 0x32, span, 1, minimum, 0xF0500, 0)
            return struct.pack('<IIIIIIII', 0xFEEDFACF, cpu, 0, 6, 1, len(command), 0, 0) + command
        path = self.root / 'fixture.dylib'
        path.write_bytes(binary())
        self.assertEqual(native.validate_macho(path)['minimum_os'], [15, 0, 0])
        for payload in (binary(cpu=0x01000007), binary(minimum=0x100000), binary(span=4096), b'not-a-mach-o'):
            path.write_bytes(payload)
            with self.subTest(length=len(payload)), self.assertRaises(ValueError):
                native.validate_macho(path)


if __name__ == '__main__':
    unittest.main()
