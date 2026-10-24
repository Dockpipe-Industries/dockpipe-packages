"""Synthetic lock admission only; these fixtures are not published candidate pins."""
import copy
import json
from pathlib import Path
import unittest
from unittest.mock import Mock, patch

import test_native_store as store_tests
import native_inputs
import native_inputs_macos
import native_store
import prepare_ci

ROOT = Path(__file__).resolve().parents[4]


class MacLaneTest(unittest.TestCase):
    def setUp(self):
        self.fixture = store_tests.NativeStoreTest(methodName='runTest')
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root = self.fixture.root
        self.recipe = json.loads((ROOT / 'release/native-inputs/darwin-arm64.json').read_text())

    def lock(self):
        document = self.fixture.lock()
        profile = document['profiles'].pop('linux-amd64')
        profile['native_inputs'] = copy.deepcopy(self.recipe)
        for role in native_inputs.DERIVED_ROLES:
            profile['inputs'].pop(role)
        document['accepted_platforms'] = ['darwin-arm64']
        document['profiles'] = {'darwin-arm64': profile}
        return document

    def test_mac_requires_complete_exact_recipe_not_direct_role_bypass(self):
        document = self.lock()
        path = self.fixture.save_lock(document)
        actual, profile = native_store.load_lock(path, 'darwin-arm64')
        self.assertEqual(actual['accepted_platforms'], ['darwin-arm64'])
        self.assertEqual(profile['native_inputs']['compiler']['version'], '17.0.0.17000013')
        for change in ('missing-recipe', 'mixed-input', 'wrong-baseline'):
            invalid = copy.deepcopy(document)
            candidate = invalid['profiles']['darwin-arm64']
            if change == 'missing-recipe':
                candidate.pop('native_inputs')
                candidate['inputs'].update({role: self.fixture.asset() for role in native_inputs.DERIVED_ROLES})
            elif change == 'mixed-input':
                candidate['inputs']['qt'] = self.fixture.asset()
            else:
                candidate['native_inputs']['compiler']['version'] = '21.0.0.21000334'
            with self.subTest(change=change), self.assertRaises(ValueError):
                native_store.load_lock(self.fixture.save_lock(invalid), 'darwin-arm64')

    def test_mac_ci_selects_actual_arm64_os15_runner_only_when_explicitly_accepted(self):
        document = self.lock()
        document['source_commit'] = None
        profile = document['profiles']['darwin-arm64']
        profile['inputs']['source'] = dict(url=None, sha256=None, size_bytes=None, format=None, root=None)
        environment = {'GITHUB_REPOSITORY': native_store.REPOSITORY, 'GITHUB_SHA': self.fixture.commit}
        def download(url, output):
            output.write_bytes(b'fixture-public-source-transport')
        matrix = prepare_ci.prepare(self.fixture.save_lock(document), self.root / 'effective', environment, download)
        self.assertEqual(matrix, {'include': [{'platform': 'darwin-arm64', 'runner': 'macos-15'}]})

    def test_native_ci_uses_os_framework_reader_and_job_local_xcode_selection(self):
        workflow = (ROOT / '.github/workflows/native-store-candidate.yml').read_text()
        self.assertIn("if: matrix.platform != 'darwin-arm64'", workflow)
        self.assertIn('DEVELOPER_DIR=/Applications/Xcode_16.4.app/Contents/Developer', workflow)
        self.assertNotIn('xcode-select', workflow)
        self.assertNotIn('native-extractor-darwin-arm64.lock.txt', workflow)
        example = json.loads((ROOT / 'release/native-store.lock.example.json').read_text())
        self.assertEqual(set(example['accepted_platforms']), set(example['profiles']))
        self.assertEqual(set(example['accepted_platforms']), {'linux-amd64', 'windows-amd64', 'darwin-arm64'})
        self.assertNotIn('linux-arm64', example['accepted_platforms'])
        self.assertEqual(example['profiles']['darwin-arm64']['native_inputs']['compiler']['os_version'], '15.0')

    def test_native_recipe_dispatch_preserves_explicit_selected_inputs(self):
        selected = (self.recipe, self.root / 'output', self.root / 'tools', self.root / 'cache', False, Mock(), {})
        marker = object()
        with patch.object(native_inputs_macos, 'prepare', return_value=marker) as prepare:
            self.assertIs(native_inputs.prepare(*selected), marker)
        prepare.assert_called_once_with(*selected)
        self.assertFalse((self.root / 'output').exists())


if __name__ == '__main__':
    unittest.main()
