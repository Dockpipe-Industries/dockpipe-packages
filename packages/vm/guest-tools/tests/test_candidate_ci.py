"""Locked acquisition tests use in-memory transport only."""
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

TOOLING = Path(__file__).resolve().parents[1] / 'tooling'
sys.path.insert(0, str(TOOLING))
specification = importlib.util.spec_from_file_location('guest_ci', TOOLING / 'candidate_ci.py')
ci = importlib.util.module_from_spec(specification)
specification.loader.exec_module(ci)


class CandidateAcquisitionTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.output = Path(self.temporary.name) / 'locked.7z'
        self.entry = {'url': 'https://download.qt.io/online/qtsdkrepository/fixture.7z',
                      'size_bytes': 3, 'sha256': ci.build_candidate.candidate.digest(b'abc')}


    def test_old_cli_pin_is_refused_before_native_tool_acquisition(self):
        root = Path(self.temporary.name)
        (root / 'dockpipe.config.json').write_text(json.dumps({'dockpipe_version': '0.6.25'}))
        (root / 'dockpipe.lock.json').write_text(json.dumps({'schema': 1, 'dockpipe': {'version': '0.6.25', 'release': 'v0.6.25'}}))
        with self.assertRaisesRegex(ValueError, 'CLI candidate pin is required'):
            ci.require_public_cli_candidate(root)

    def test_cli_candidate_requires_exact_public_windows_triplet_and_identity(self):
        root = Path(self.temporary.name)
        source = 'a' * 40
        version = '0.6.26-staging.cli.sha' + source[:12]
        release = 'cli-candidate-0.6.26-staging.' + source
        entry = {'url': 'https://github.com/Dockpipe-Industries/dockpipe/releases/download/' + release
                        + '/dockpipe_' + version + '_windows_amd64.zip',
                 'size_bytes': 100, 'sha256': 'b' * 64, 'executable': 'dockpipe.exe', 'executable_size_bytes': 200}
        lock = {'schema': 1, 'dockpipe': {'version': version, 'release': release,
                                        'artifacts': {'windows/amd64': entry}}}
        (root / 'dockpipe.config.json').write_text(json.dumps({'dockpipe_version': version}))
        path = root / 'dockpipe.lock.json'
        path.write_text(json.dumps(lock))
        self.assertEqual(ci.require_public_cli_candidate(root), entry)
        for key, value in (('url', 'https://example.invalid/cli.zip'), ('sha256', None), ('size_bytes', True)):
            changed = json.loads(json.dumps(lock))
            changed['dockpipe']['artifacts']['windows/amd64'][key] = value
            path.write_text(json.dumps(changed))
            with self.subTest(key=key), self.assertRaises(ValueError):
                ci.require_public_cli_candidate(root)

    def test_older_cli_candidate_base_does_not_claim_new_native_features(self):
        root = Path(self.temporary.name)
        source = 'a' * 40
        for base in ('0.6.0', '0.6.25', '0.6.026'):
            version = base + '-staging.cli.sha' + source[:12]
            release = 'cli-candidate-' + base + '-staging.' + source
            (root / 'dockpipe.config.json').write_text(json.dumps({'dockpipe_version': version}))
            (root / 'dockpipe.lock.json').write_text(json.dumps({
                'schema': 1, 'dockpipe': {'version': version, 'release': release}}))
            with self.subTest(base=base), self.assertRaisesRegex(ValueError, 'CLI candidate pin is required'):
                ci.require_public_cli_candidate(root)

    def test_locked_official_bytes_only(self):
        with mock.patch.object(ci.urllib.request, 'urlopen', return_value=io.BytesIO(b'abc')):
            ci.download(self.entry, self.output)
        self.assertEqual(self.output.read_bytes(), b'abc')

    def test_truncated_oversized_or_changed_archive_refused(self):
        for content in (b'ab', b'abcd', b'abx'):
            with self.subTest(content=content), \
                    mock.patch.object(ci.urllib.request, 'urlopen', return_value=io.BytesIO(content)):
                with self.assertRaises(ValueError):
                    ci.download(self.entry, self.output)
            self.output.unlink(missing_ok=True)

    def test_nonofficial_unbounded_and_existing_destination_refused_before_transport(self):
        for change in ({'url': 'https://example.invalid/payload'}, {'size_bytes': True},
                       {'size_bytes': 257 << 20}, {'sha256': 'unlocked'}):
            with self.subTest(change=change), mock.patch.object(ci.urllib.request, 'urlopen') as transport:
                with self.assertRaises(ValueError):
                    ci.download(self.entry | change, self.output)
                transport.assert_not_called()
        self.output.write_bytes(b'preserved')
        with self.assertRaises(ValueError):
            ci.download(self.entry, self.output)
        self.assertEqual(self.output.read_bytes(), b'preserved')


if __name__ == '__main__':
    unittest.main()
