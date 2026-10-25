import copy
import hashlib
import json
from pathlib import Path
import unittest
from unittest.mock import patch

import test_native_store as store_tests
import native_store
import prepare_ci


class PrepareCITest(unittest.TestCase):
    def setUp(self):
        self.fixture = store_tests.NativeStoreTest(methodName='runTest')
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root = self.fixture.root
        self.environment = {'GITHUB_REPOSITORY': native_store.REPOSITORY,
                            'GITHUB_SHA': self.fixture.commit}

    def template(self):
        document = self.fixture.lock()
        document['source_commit'] = None
        document['profiles']['linux-amd64']['inputs']['source'] = dict(
            url=None, sha256=None, size_bytes=None, format=None, root=None)
        return document

    def download(self, url, output):
        self.assertEqual(url, 'https://github.com/' + native_store.REPOSITORY
                         + '/archive/' + self.fixture.commit + '.tar.gz')
        output.write_bytes(b'exact-public-committed-source-transport')

    def test_effective_source_identity_is_computed_once_after_commit_exists(self):
        path = self.fixture.save_lock(self.template())
        original = path.read_bytes()
        output = self.root / 'effective'
        matrix = prepare_ci.prepare(path, output, self.environment, self.download)
        self.assertEqual(path.read_bytes(), original)
        self.assertEqual(matrix, {'include': [{'platform': 'linux-amd64', 'runner': 'ubuntu-24.04'}]})
        document, profile = native_store.load_lock(output / 'effective-lock.json', 'linux-amd64')
        self.assertEqual(document['source_commit'], self.fixture.commit)
        source = profile['inputs']['source']
        self.assertEqual(source['sha256'], hashlib.sha256((output / 'source-archive.tar.gz').read_bytes()).hexdigest())
        self.assertEqual(source['size_bytes'], (output / 'source-archive.tar.gz').stat().st_size)
        self.assertEqual(source['root'], 'dockpipe-packages-' + self.fixture.commit)

    def test_explicit_platform_subset_shares_one_transport_identity(self):
        document = self.template()
        document['accepted_platforms'].append('windows-amd64')
        document['profiles']['windows-amd64'] = copy.deepcopy(document['profiles']['linux-amd64'])
        downloads = []
        def download(url, output):
            downloads.append(url)
            self.download(url, output)
        output = self.root / 'effective'
        matrix = prepare_ci.prepare(self.fixture.save_lock(document), output, self.environment, download)
        effective = native_store.read_json(output / 'effective-lock.json')
        self.assertEqual(len(downloads), 1)
        self.assertEqual(len(matrix['include']), 2)
        self.assertEqual(effective['profiles']['linux-amd64']['inputs']['source'],
                         effective['profiles']['windows-amd64']['inputs']['source'])

    def test_missing_dependency_fails_before_source_download_or_output(self):
        document = self.template()
        document['profiles']['linux-amd64']['inputs']['broker']['sha256'] = None
        output = self.root / 'effective'
        with patch.object(prepare_ci, 'download_source') as downloader:
            with self.assertRaises(ValueError):
                prepare_ci.prepare(self.fixture.save_lock(document), output, self.environment, downloader)
        downloader.assert_not_called()
        self.assertFalse(output.exists())

    def test_source_self_pins_and_foreign_ci_identity_fail(self):
        for document in (self.fixture.lock(), self.template()):
            if document['source_commit'] is None:
                document['profiles']['linux-amd64']['inputs']['source']['sha256'] = 'b' * 64
            with self.subTest(document=document), self.assertRaises(ValueError):
                prepare_ci.prepare(self.fixture.save_lock(document), self.root / 'effective', self.environment, self.download)
        for key, value in (('GITHUB_REPOSITORY', 'Other/Repository'), ('GITHUB_SHA', 'dev')):
            environment = dict(self.environment, **{key: value})
            with self.subTest(key=key), self.assertRaises(ValueError):
                prepare_ci.prepare(self.fixture.save_lock(self.template()), self.root / 'effective', environment, self.download)

    def test_existing_output_and_unqualified_platform_fail(self):
        output = self.root / 'effective'
        output.mkdir()
        with self.assertRaises(ValueError):
            prepare_ci.prepare(self.fixture.save_lock(self.template()), output, self.environment, self.download)
        document = self.template()
        document['accepted_platforms'] = ['darwin-arm64']
        document['profiles'] = {'darwin-arm64': document['profiles']['linux-amd64']}
        with self.assertRaises(ValueError):
            prepare_ci.prepare(self.fixture.save_lock(document), self.root / 'missing', self.environment, self.download)

    def test_ci_has_native_dependency_gate_and_only_accepted_push_can_write(self):
        path = Path(__file__).resolve().parents[4] / '.github/workflows/native-store-candidate.yml'
        source = path.read_text()
        self.assertIn('needs: [inputs, native]', source)
        self.assertIn("github.event_name == 'push' && github.ref == 'refs/heads/dev'", source)
        self.assertEqual(source.count('contents: write'), 1)
        self.assertNotIn('tooling/bootstrap_runtime_core.py', source)
        self.assertIn('effective-lock.json', source)
        self.assertIn('publish_candidate.py --candidate-dir', source)
        self.assertNotIn('pull_request_target', source)
        self.assertNotIn('cancel-in-progress: true', source)


if __name__ == '__main__':
    unittest.main()
