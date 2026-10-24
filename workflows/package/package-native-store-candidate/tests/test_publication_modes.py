"""Qualification modes own disjoint immutable releases, even for one source commit."""
import json
from pathlib import Path
import unittest

import test_native_store
import catalog_transaction
import publication_plan


class PublicationModeTests(unittest.TestCase):
    def setUp(self):
        self.fixture = test_native_store.NativeStoreTest(methodName='runTest')
        self.fixture.setUp()
        self.addCleanup(self.fixture.temporary.cleanup)

    def test_same_source_and_asset_name_cannot_collide_between_modes(self):
        candidate, archive = self.fixture.candidate()
        native = publication_plan.plan([candidate], qualification='native')
        descriptor = json.loads(candidate.read_text())
        descriptor.update(publication_mode='source-only', native_build=False, source_only=True,
                          tag='catalog-candidate-' + self.fixture.commit)
        candidate.write_text(json.dumps(descriptor))
        source = publication_plan.plan([candidate], qualification='source-only')

        self.assertEqual(source['tag'], 'catalog-candidate-' + self.fixture.commit)
        self.assertEqual(native['tag'], 'catalog-native-candidate-' + self.fixture.commit)
        self.assertNotEqual(source['tag'], native['tag'])
        source_asset = source['artifacts']['linux-amd64']
        native_asset = native['artifacts']['linux-amd64']
        self.assertEqual(source_asset['sha256'], native_asset['sha256'])
        self.assertEqual(Path(source_asset['url']).name, archive.name)
        self.assertEqual(Path(native_asset['url']).name, archive.name)
        self.assertNotEqual(source_asset['url'], native_asset['url'])
        with self.assertRaises(ValueError):
            publication_plan.plan([candidate], qualification='native')
        with self.assertRaises(TypeError):
            publication_plan.plan([candidate])

    def test_descriptor_mode_and_tag_are_both_required_before_publication(self):
        candidate, archive = self.fixture.candidate()
        original = json.loads(candidate.read_text())
        for field, value in (
                ('publication_mode', None),
                ('publication_mode', 'source-only'),
                ('tag', 'catalog-candidate-' + self.fixture.commit)):
            with self.subTest(field=field, value=value):
                candidate.write_text(json.dumps({**original, field: value}))
                with self.assertRaises(ValueError):
                    publication_plan.plan([candidate], qualification='native')

    def test_transaction_rejects_cross_mode_receipts_before_output_or_network(self):
        source = self.fixture.commit
        valid = {
            'source_commit': source,
            'publication_mode': 'native',
            'artifact_class': 'native-store-overlay',
            'native_build': True,
            'tag': 'catalog-native-candidate-' + source,
        }
        for changed in (
                {'publication_mode': 'source-only'},
                {'artifact_class': 'source-only-store-overlay'},
                {'native_build': False},
                {'tag': 'catalog-candidate-' + source}):
            with self.subTest(changed=changed):
                output = self.fixture.root / 'must-not-exist'
                # An object without client methods proves no remote operation was attempted.
                with self.assertRaises(ValueError):
                    catalog_transaction.publish_assets(
                        {**valid, **changed}, {}, output, 'native-store-release.json', object(),
                        qualification='native')
                self.assertFalse(output.exists())
        with self.assertRaises(ValueError):
            catalog_transaction.release_tag('guessed-from-filename', source)


if __name__ == '__main__':
    unittest.main()
