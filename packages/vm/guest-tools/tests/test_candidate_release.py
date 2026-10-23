"""Synthetic fixtures exercise admission; they do not constitute native proof."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

TOOLING = Path(__file__).resolve().parents[1] / 'tooling'
sys.path.insert(0, str(TOOLING))
specification = importlib.util.spec_from_file_location('guest_candidate', TOOLING / 'prepare_candidate.py')
candidate = importlib.util.module_from_spec(specification)
specification.loader.exec_module(candidate)
SOURCE = '1' * 40


def encoded(value):
    return (json.dumps(value, indent=2) + '\n').encode()


def write_document(path, value):
    path.write_bytes(encoded(value))


class CandidateReleaseTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.inputs = self.root / 'inputs'
        self.inputs.mkdir()
        lock = {'schema': 1, 'target': candidate.PLATFORM, 'qt_version': '6.10.3'}
        lock_bytes = encoded(lock)
        self.path('guest_toolchain_lock').write_bytes(lock_bytes)
        source_inventory = [{'path': 'tooling/toolchain.lock.json', 'sha256': candidate.digest(lock_bytes)}]
        write_document(self.path('guest_source_inventory'), source_inventory)
        inventory_sha = candidate.source_inventory(source_inventory)[1]
        files = {'dockpipe-guest-tools.exe': b'synthetic unit-test executable, not native evidence',
                 'install-guest-tools.ps1': b'synthetic installer', 'licenses/THIRD-PARTY.txt': b'fixture license'}
        inventory = {'schema': 1, 'product': 'DockpipeGuestTools', 'version': '1.0.0',
                     'platform': candidate.PLATFORM, 'qt_version': '6.10.3',
                     'files': [{'path': name, 'bytes': len(data), 'sha256': candidate.digest(data)}
                               for name, data in files.items()]}
        inventory_bytes = encoded(inventory)
        with zipfile.ZipFile(self.path('guest_tools'), 'w') as archive:
            for name, data in files.items():
                archive.writestr(name, data)
            archive.writestr('inventory.json', inventory_bytes)
        archive_bytes = self.path('guest_tools').read_bytes()
        self.descriptor = {'schema': 1, 'product': 'DockpipeGuestTools', 'version': '1.0.0',
                           'platform': candidate.PLATFORM, 'qt_version': '6.10.3',
                           'entrypoint': 'dockpipe-guest-tools.exe', 'installer': 'install-guest-tools.ps1',
                           'media_subdirectory': 'GuestTools', 'inventory_sha256': candidate.digest(inventory_bytes),
                           'source_inventory_sha256': inventory_sha,
                           'archive': {'name': candidate.NAMES['guest_tools'], 'size_bytes': len(archive_bytes),
                                       'sha256': candidate.digest(archive_bytes)}}
        write_document(self.path('guest_descriptor'), self.descriptor)
        self.provenance = {
            'schema': 1, 'product': 'DockpipeGuestToolsNativeBuild',
            'source_repository': candidate.REPOSITORY, 'source_sha': SOURCE,
            'source_state': 'committed-snapshot', 'native_host': candidate.PLATFORM,
            'qualification_origin': 'github-actions-native-windows',
            'source_inventory_sha256': inventory_sha, 'toolchain_lock_sha256': candidate.digest(lock_bytes),
            'archive_sha256': candidate.digest(archive_bytes),
            'descriptor_sha256': candidate.digest(self.path('guest_descriptor').read_bytes()),
            **candidate.TOOL_VERSIONS,
            'checks': list(candidate.CHECKS),
            'ctest_tests': {'guest-tools-status': 'passed', 'guest-tools-clipboard': 'passed'},
            'installer_integrity': {'installer_parse': 'passed', 'corrupt_executable_rejected': True,
                                    'ui_task_unchanged': True},
            'workflow': {'repository': candidate.REPOSITORY, 'run_id': 123,
                         'run_attempt': 1, 'workflow_sha': '2' * 40},
        }
        self.save_provenance()

    def path(self, role):
        return self.inputs / candidate.NAMES[role]

    def save_provenance(self):
        write_document(self.path('guest_native_provenance'), self.provenance)

    def reject(self, expression=''):
        with self.assertRaisesRegex(ValueError, expression):
            candidate.manifest_for(self.inputs, SOURCE)

    def test_prepares_exact_assets_and_nonpublishing_immutable_plan(self):
        output = self.root / 'prepared'
        manifest = candidate.prepare(self.inputs, output, SOURCE)
        self.assertEqual(manifest['tag'], 'guest-tools-candidate-' + SOURCE)
        self.assertEqual(set(manifest['artifacts']), set(candidate.NAMES))
        for role, entry in manifest['artifacts'].items():
            self.assertEqual(entry['url'], 'https://github.com/' + candidate.REPOSITORY
                             + '/releases/download/' + manifest['tag'] + '/' + entry['name'])
            self.assertEqual(hashlib.sha256((output / 'assets' / entry['name']).read_bytes()).hexdigest(), entry['sha256'])
        self.assertEqual({path.name for path in (output / 'assets').iterdir()},
                         set(candidate.NAMES.values()) | {candidate.MANIFEST_NAME})
        plan = json.loads((output / 'publication-plan.json').read_text())
        self.assertTrue(plan['draft'])
        self.assertTrue(plan['prerelease'])
        self.assertFalse(plan['latest'])
        self.assertFalse(plan['publication_performed'])
        self.assertTrue(plan['public_readback_required'])

    def test_rejects_development_or_non_native_provenance(self):
        for field, value in [('source_state', 'development'), ('native_host', 'linux-amd64'),
                             ('qualification_origin', 'reviewed-local-build')]:
            with self.subTest(field=field):
                original = self.provenance[field]
                self.provenance[field] = value
                self.save_provenance()
                self.reject('Native provenance differs')
                self.provenance[field] = original

    def test_binds_actual_source_and_each_selected_tool(self):
        for field, value in [('source_sha', '3' * 40), ('compiler_version', '13.2.0'),
                             ('qt_version', '6.10.2'), ('cmake_version', '3.31.0'),
                             ('ninja_version', '1.13.0')]:
            with self.subTest(field=field):
                original = self.provenance[field]
                self.provenance[field] = value
                self.save_provenance()
                self.reject('Native provenance differs')
                self.provenance[field] = original

    def test_requires_actual_named_ctests_and_installer_rejection(self):
        self.provenance['ctest_tests']['guest-tools-clipboard'] = 'skipped'
        self.save_provenance()
        self.reject('both CTests')
        self.provenance['ctest_tests']['guest-tools-clipboard'] = 'passed'
        self.provenance['installer_integrity']['corrupt_executable_rejected'] = 1
        self.save_provenance()
        self.reject('typed booleans')

    def test_rejects_missing_hosted_identity_and_boolean_schema(self):
        self.provenance['workflow']['run_id'] = True
        self.save_provenance()
        self.reject('hosted workflow identity')
        self.provenance['workflow']['run_id'] = 123
        self.provenance['schema'] = True
        self.save_provenance()
        self.reject('Native provenance differs')

    def test_rejects_archive_and_descriptor_drift(self):
        self.path('guest_tools').write_bytes(self.path('guest_tools').read_bytes() + b'tampered')
        self.reject('Native provenance differs')

    def test_rejects_source_inventory_or_toolchain_pin_drift(self):
        inventory = json.loads(self.path('guest_source_inventory').read_text())
        inventory[0]['sha256'] = 'f' * 64
        write_document(self.path('guest_source_inventory'), inventory)
        self.reject('Source inventory differs')

    def test_rejects_toolchain_bytes_outside_source_inventory(self):
        lock = json.loads(self.path('guest_toolchain_lock').read_text())
        lock['extra'] = 'changed'
        write_document(self.path('guest_toolchain_lock'), lock)
        self.reject('toolchain lock is not')

    def test_rejects_duplicate_and_oversized_metadata(self):
        self.path('guest_descriptor').write_text('{"schema":1,"schema":1}')
        self.reject('Duplicate metadata key')
        self.path('guest_descriptor').write_bytes(b' ' * 65537)
        self.reject('bounded regular file')

    def test_rejects_metadata_symlinks_and_unlisted_input_assets(self):
        saved = self.root / 'descriptor-saved.json'
        self.path('guest_descriptor').rename(saved)
        self.path('guest_descriptor').symlink_to(saved)
        self.reject('bounded regular file')
        self.path('guest_descriptor').unlink()
        saved.rename(self.path('guest_descriptor'))
        (self.inputs / 'unreviewed.txt').write_text('unlisted')
        self.reject('exactly the five')

    def test_rejects_case_collisions_and_unsafe_source_paths(self):
        value = [{'path': 'A', 'sha256': 'a' * 64}, {'path': 'a', 'sha256': 'b' * 64}]
        with self.assertRaisesRegex(ValueError, 'Duplicate source'):
            candidate.source_inventory(value)
        with self.assertRaisesRegex(ValueError, 'unsafe relative path'):
            candidate.source_inventory([{'path': '../escape', 'sha256': 'a' * 64}])

    def test_preserves_existing_output_without_overwrite(self):
        output = self.root / 'existing'
        output.mkdir()
        marker = output / 'preserve'
        marker.write_bytes(b'original')
        with self.assertRaisesRegex(ValueError, 'fresh output'):
            candidate.prepare(self.inputs, output, SOURCE)
        self.assertEqual(marker.read_bytes(), b'original')

    def test_prepared_bytes_use_the_validated_snapshot(self):
        original = candidate.validate_inputs
        before = self.path('guest_tools').read_bytes()

        def mutate_after_admission(*arguments):
            result = original(*arguments)
            self.path('guest_tools').write_bytes(b'changed after admission')
            return result

        output = self.root / 'snapshot'
        with mock.patch.object(candidate, 'validate_inputs', side_effect=mutate_after_admission):
            candidate.prepare(self.inputs, output, SOURCE)
        self.assertEqual((output / 'assets' / candidate.NAMES['guest_tools']).read_bytes(), before)

    def remote_fixture(self):
        output = self.root / 'remote-prepared'
        manifest = candidate.prepare(self.inputs, output, SOURCE)
        plan = json.loads((output / 'publication-plan.json').read_text())
        release = {'tag_name': manifest['tag'], 'target_commitish': SOURCE, 'prerelease': True,
                   'draft': False, 'assets': [{'name': name, 'size': item['size_bytes'],
                                             'digest': 'sha256:' + item['sha256']}
                                            for name, item in plan['assets'].items()]}
        return manifest, plan['assets'], release

    def test_immutable_remote_readback_reuses_only_exact_bytes(self):
        manifest, assets, release = self.remote_fixture()
        self.assertEqual(candidate.validate_remote_release(release, SOURCE, manifest, assets),
                         'reuse-identical-candidate')
        release['assets'][0]['digest'] = 'sha256:' + 'f' * 64
        with self.assertRaisesRegex(ValueError, 'never clobber'):
            candidate.validate_remote_release(release, SOURCE, manifest, assets)

    def test_rejects_wrong_tag_missing_digest_and_remote_inventory(self):
        manifest, assets, release = self.remote_fixture()
        with self.assertRaisesRegex(ValueError, 'different source'):
            candidate.validate_remote_release(release, 'f' * 40, manifest, assets)
        for changed in [release['assets'][:-1], release['assets'] + [copy.deepcopy(release['assets'][0])]]:
            alternate = dict(release, assets=changed)
            with self.assertRaisesRegex(ValueError, 'inventory differs'):
                candidate.validate_remote_release(alternate, SOURCE, manifest, assets)
        release['assets'][0].pop('digest')
        with self.assertRaisesRegex(ValueError, 'never clobber'):
            candidate.validate_remote_release(release, SOURCE, manifest, assets)

    def test_missing_tag_is_allowed_only_before_qualified_draft_publication(self):
        manifest, assets, release = self.remote_fixture()
        with self.assertRaisesRegex(ValueError, 'resolved source tag'):
            candidate.validate_remote_release(release, None, manifest, assets)
        release['draft'] = True
        self.assertEqual(candidate.validate_remote_release(release, None, manifest, assets),
                         'publish-qualified-draft')
        self.assertEqual(candidate.validate_remote_release(None, None, manifest, assets), 'create-draft')


if __name__ == '__main__':
    unittest.main()
