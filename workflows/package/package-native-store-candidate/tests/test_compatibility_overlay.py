"""A native candidate must keep all qualified source repairs byte-for-byte."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import yaml

import test_native_store as fixtures
import compatibility_overlay
import native_store


class CompatibilityOverlayTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'qualified'
        self.source.mkdir()
        self.store = self.root / 'native-workflows'
        self.store.mkdir()
        self.reference = {'source_commit': 'a' * 40, 'owned': compatibility_overlay.OWNED,
            'asset': {'url': 'https://github.com/Dockpipe-Industries/dockpipe-packages/releases/download/'
                            + 'catalog-candidate-' + 'a' * 40 + '/dockpipe-catalog-linux-amd64.tar.gz',
                      'sha256': 'b' * 64, 'size_bytes': 10, 'format': 'tar.gz', 'root': ''}}
        entries = []
        for kind, name in compatibility_overlay.OWNED:
            data = ('qualified source ' + name).encode()
            archive = 'dockpipe-workflow-' + name + '-1.0.0.tar.gz'
            (self.source / archive).write_bytes(data)
            entries.append({'name': name, 'version': '1.0.0', 'tarball': archive,
                            'sha256': hashlib.sha256(data).hexdigest(), 'size_bytes': len(data)})
        self.manifest = {'schema': 1, 'platform': 'linux-amd64', 'packages': {'workflows': entries}}
        self.save()
        (self.store / 'packages-store-manifest.json').write_text(json.dumps({
            'schema': 1, 'platform': 'windows-amd64', 'packages': {'workflows': []}}))

    def save(self):
        (self.source / 'packages-store-manifest.json').write_text(json.dumps(self.manifest))

    def test_union_retargets_only_outer_store_and_preserves_all_source_archive_bytes(self):
        compatibility_overlay.validate(self.reference, native_store.validate_asset)
        before = {path.name: path.read_bytes() for path in self.source.iterdir()}
        compatibility_overlay.merge(self.reference, self.source, self.store)
        native_store.combine({'workflow': self.store}, compatibility_overlay.OWNED, 'windows-amd64',
                             self.root / 'union', {})
        result = json.loads((self.root / 'union/packages-store-manifest.json').read_text())
        self.assertEqual(result['platform'], 'windows-amd64')
        self.assertEqual(result['packages']['workflows'], self.manifest['packages']['workflows'])
        for entry in result['packages']['workflows']:
            self.assertEqual((self.root / 'union' / entry['tarball']).read_bytes(), before[entry['tarball']])
        self.assertEqual(before, {path.name: path.read_bytes() for path in self.source.iterdir()})

    def test_missing_identity_tampered_bytes_and_unlisted_payload_fail(self):
        for mutation in ('identity', 'bytes', 'extra'):
            with self.subTest(mutation=mutation):
                if mutation == 'identity':
                    self.manifest['packages']['workflows'][0]['name'] = 'different'
                    self.save()
                elif mutation == 'bytes':
                    self.manifest['packages']['workflows'][0]['name'] = compatibility_overlay.OWNED[0][1]
                    self.save()
                    (self.source / self.manifest['packages']['workflows'][0]['tarball']).write_bytes(b'changed')
                else:
                    (self.source / 'unlisted').write_bytes(b'extra')
                with self.assertRaises(ValueError):
                    compatibility_overlay.merge(self.reference, self.source, self.store)

    def test_example_preserves_all_five_new_vm_installables_and_three_source_repairs(self):
        catalog = Path(__file__).resolve().parents[4]
        example = json.loads((catalog / 'release/native-store.lock.example.json').read_text())
        expected = compatibility_overlay.OWNED + [['resolver', 'qemu'], ['workflow', 'linux-vm'],
            ['workflow', 'windows-vm'], ['workflow', 'vm-manage'], ['workflow', 'vm-resolution']]
        self.assertEqual(sorted(example['owned']), sorted(expected))
        self.assertEqual(len(example['owned']), 8)
        for profile in example['profiles'].values():
            self.assertEqual(profile['compatibility_overlay']['owned'], compatibility_overlay.OWNED)

    def test_undeclared_union_ownership_or_source_tag_cannot_be_substituted(self):
        reference = copy.deepcopy(self.reference)
        reference['owned'] = reference['owned'][:2]
        with self.assertRaises(ValueError):
            compatibility_overlay.validate(reference, native_store.validate_asset)
        reference = copy.deepcopy(self.reference)
        reference['source_commit'] = 'c' * 40
        with self.assertRaises(ValueError):
            compatibility_overlay.validate(reference, native_store.validate_asset)

    def test_native_family_requires_all_five_coherent_versions_and_runtime_minimums(self):
        manifest = {'packages': {'workflows': [], 'resolvers': []}}
        for kind, name in native_store.VM_FAMILY:
            manifest['packages'][kind + 's'].append({'name': name, 'version': '1.3.7',
                                                   'min_dockpipe_version': '0.6.29-0'})
        native_store.verify_vm_family(manifest)
        manifest['packages']['workflows'][0]['version'] = '1.3.5'
        with self.assertRaisesRegex(ValueError, 'coherent'):
            native_store.verify_vm_family(manifest)
        manifest['packages']['workflows'][0]['version'] = '1.3.7'
        manifest['packages']['workflows'].pop()
        with self.assertRaisesRegex(ValueError, 'all five'):
            native_store.verify_vm_family(manifest)

    def test_actual_source_cohort_is_admitted_and_previous_runtime_floor_rejected(self):
        catalog = Path(__file__).resolve().parents[4]
        family = catalog / 'packages/vm'
        manifest = {'packages': {'workflows': [], 'resolvers': []}}
        for kind, name in sorted(native_store.VM_FAMILY):
            directory = family / 'resolvers/qemu' if kind == 'resolver' else family / 'workflows' / name
            source = yaml.safe_load((directory / 'package.yml').read_text())
            self.assertEqual(source['min_dockpipe_version'], '0.6.29-0')
            manifest['packages'][kind + 's'].append(source)
        native_store.verify_vm_family(manifest)
        for group in manifest['packages'].values():
            for artifact in group:
                original = artifact['min_dockpipe_version']
                artifact['min_dockpipe_version'] = '0.6.28-0'
                with self.subTest(name=artifact['name']):
                    with self.assertRaisesRegex(ValueError, 'coherent'):
                        native_store.verify_vm_family(manifest)
                artifact['min_dockpipe_version'] = original

    def test_existing_native_identity_refuses_overwrite(self):
        path = self.store / 'packages-store-manifest.json'
        path.write_text(json.dumps({'schema': 1, 'platform': 'windows-amd64', 'packages': {
            'workflows': [self.manifest['packages']['workflows'][0]]}}))
        with self.assertRaisesRegex(ValueError, 'ownership overlap'):
            compatibility_overlay.merge(self.reference, self.source, self.store)


if __name__ == '__main__':
    unittest.main()
