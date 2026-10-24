"""Toolchain source binding, extraction and native dispatcher failure boundaries."""
import copy
import hashlib
import io
import json
from pathlib import Path
import shutil
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import test_native_store as fixtures
import native_store
import toolchain_inputs


class ToolchainInputsTest(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.NativeStoreTest(methodName='runTest')
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root = self.fixture.root
        self.catalog = Path(__file__).resolve().parents[4]
        self.profiles = {target: json.loads((self.catalog / 'release/native-toolchain' / (target + '.json')).read_text())
                         for target in ('linux-amd64', 'windows-amd64')}

    def reference(self, target='linux-amd64'):
        path = self.root / 'profile.json'
        path.write_text(json.dumps(self.profiles[target]))
        return {'path': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}

    def test_native_workflow_parses_and_installs_both_hash_locked_contract_dependencies(self):
        import yaml
        document = yaml.load((self.catalog / '.github/workflows/native-store-candidate.yml').read_text(), Loader=yaml.BaseLoader)
        inputs = document['jobs']['inputs']['steps']
        installs = next(step['run'] for step in inputs if step.get('name') == 'Install the exact native extraction implementation')
        self.assertIn('--require-hashes', installs)
        self.assertIn('release/native-extractor-linux-amd64.lock.txt', installs)
        self.assertIn('release/source-overlay-requirements.lock.txt', installs)

    def test_exact_official_profiles_preserve_gnu_and_frozen_msvc_baselines(self):
        for target, profile in self.profiles.items():
            toolchain_inputs.validate_profile(profile, target)
        self.assertEqual(len(self.profiles['linux-amd64']['compiler_inputs']), 18)
        self.assertEqual(self.profiles['windows-amd64']['compiler']['version'], '19.51.36260.0')

    def test_toolchain_recipe_and_direct_input_are_mutually_exclusive(self):
        lock = self.fixture.lock()
        profile = lock['profiles']['linux-amd64']
        profile['toolchain_recipe'] = self.reference()
        with self.assertRaises(ValueError):
            native_store.load_lock(self.fixture.save_lock(lock), 'linux-amd64')
        del profile['inputs']['toolchain']
        native_store.load_lock(self.fixture.save_lock(lock), 'linux-amd64')
        del profile['toolchain_recipe']
        with self.assertRaises(ValueError):
            native_store.load_lock(self.fixture.save_lock(lock), 'linux-amd64')

    def test_tampered_profile_symlink_and_traversal_are_rejected_before_native_commands(self):
        reference = self.reference()
        toolchain_inputs.load_profile(self.root, reference, 'linux-amd64')
        (self.root / 'profile.json').write_text('{}')
        with self.assertRaises(ValueError):
            toolchain_inputs.load_profile(self.root, reference, 'linux-amd64')
        with self.assertRaises(ValueError):
            toolchain_inputs.validate_reference({'path': '../profile.json', 'sha256': 'a' * 64})
        (self.root / 'alias').symlink_to('profile.json')
        reference['path'] = 'alias'
        with self.assertRaises(ValueError):
            toolchain_inputs.load_profile(self.root, reference, 'linux-amd64')

    def test_wrong_version_duplicate_tool_foreign_origin_and_unbounded_size_fail(self):
        for mutation in ('version', 'duplicate', 'origin', 'size', 'compiler'):
            with self.subTest(mutation=mutation):
                profile = copy.deepcopy(self.profiles['windows-amd64'])
                if mutation == 'version':
                    profile['inputs'][0]['version'] = 'latest'
                elif mutation == 'duplicate':
                    profile['inputs'][1] = profile['inputs'][0]
                elif mutation == 'origin':
                    profile['inputs'][0]['url'] = 'https://example.invalid/go.zip'
                elif mutation == 'size':
                    profile['inputs'][0]['size_bytes'] = True
                else:
                    profile['compiler'] = None
                with self.assertRaises(ValueError):
                    toolchain_inputs.validate_profile(profile, 'windows-amd64')

    def test_zip_handles_directories_and_exact_root_without_link_or_case_collisions(self):
        archive = self.root / 'input.zip'
        with zipfile.ZipFile(archive, 'w') as stream:
            stream.writestr('locked/', b'')
            stream.writestr('locked/bin/', b'')
            stream.writestr('locked/bin/tool.exe', b'owned tool')
        toolchain_inputs.unpack({'format': 'zip', 'root': 'locked'}, archive, self.root / 'good')
        self.assertEqual((self.root / 'good/bin/tool.exe').read_bytes(), b'owned tool')
        cases = [('locked/../escape', False), ('locked/link', True), ('other/file', False)]
        for number, (name, link) in enumerate(cases):
            with zipfile.ZipFile(archive, 'w') as stream:
                item = zipfile.ZipInfo(name)
                if link:
                    item.external_attr = 0o120777 << 16
                stream.writestr(item, b'bad')
            with self.assertRaises(ValueError):
                toolchain_inputs.unpack({'format': 'zip', 'root': 'locked'}, archive, self.root / str(number))

    def test_case_colliding_zip_entries_never_replace_previous_material(self):
        archive = self.root / 'collision.zip'
        with zipfile.ZipFile(archive, 'w') as stream:
            stream.writestr('locked/A', b'original')
            stream.writestr('locked/a', b'changed')
        with self.assertRaisesRegex(ValueError, 'case-colliding'):
            toolchain_inputs.unpack({'format': 'zip', 'root': 'locked'}, archive, self.root / 'collision')
        self.assertEqual((self.root / 'collision/A').read_bytes(), b'original')

    def test_tar_rejects_links_and_reused_output(self):
        archive = self.root / 'input.tar.gz'
        with tarfile.open(archive, 'w:gz') as stream:
            item = tarfile.TarInfo('locked/link')
            item.type = tarfile.SYMTYPE
            item.linkname = '../outside'
            stream.addfile(item)
        with self.assertRaises(ValueError):
            toolchain_inputs.unpack({'format': 'tar.gz', 'root': 'locked'}, archive, self.root / 'bad')
        with self.assertRaises(FileExistsError):
            toolchain_inputs.unpack({'format': 'tar.gz', 'root': 'locked'}, archive, self.root / 'bad')

    def test_wrong_host_and_disabled_fetch_do_not_start_toolchain_preparation(self):
        reference = self.reference('windows-amd64')
        with patch.object(toolchain_inputs.platform, 'system', return_value='Linux'):
            with patch.object(toolchain_inputs, 'prepare_windows') as prepare:
                with self.assertRaisesRegex(ValueError, 'native host'):
                    toolchain_inputs.prepare(self.root, reference, 'windows-amd64', self.root / 'prefix',
                                             self.root / 'cache', False, {})
                prepare.assert_not_called()
        reference = self.reference('linux-amd64')
        with patch.object(toolchain_inputs.platform, 'system', return_value='Linux'), \
                patch.object(toolchain_inputs.platform, 'freedesktop_os_release',
                             return_value={'ID': 'ubuntu', 'VERSION_ID': '22.04'}):
            with self.assertRaisesRegex(ValueError, 'fetching is disabled'):
                toolchain_inputs.prepare(self.root, reference, 'linux-amd64', self.root / 'prefix',
                                         self.root / 'cache', False, {})

    def test_windows_dispatch_builds_pkgconf_with_exact_offline_wheel_and_msvc_environment(self):
        commands = []
        profile = self.profiles['windows-amd64']
        prefix = self.root / 'prefix'
        compiler = self.root / 'Program Files/Microsoft Visual Studio/18/Enterprise/cl.exe'
        compiler.parent.mkdir(parents=True)
        compiler.write_bytes(b'compiler command fixture; not executed')
        def unpack(entry, archive, destination):
            destination.mkdir(parents=True)
            (destination / (entry['id'] + '.marker')).write_bytes(b'official fixture')
        def run(arguments, environment):
            commands.append([str(item) for item in arguments])
            self.assertEqual(environment['CC'], str(compiler))
            self.assertEqual(environment['CXX'], str(compiler))
            self.assertTrue(Path(environment['CC']).is_file())
            if 'install' in arguments and '-C' in arguments:
                binary = prefix / 'pkgconf/bin/pkgconf.exe'
                binary.parent.mkdir(parents=True)
                binary.write_bytes(b'compiled fixture')
            return ''
        wheel = self.root / 'wheel'
        wheel.write_bytes(b'pinned fixture')
        with patch.object(toolchain_inputs.native_inputs, 'compiler_environment',
                          return_value={'PATH': 'approved-msvc', 'CC': str(compiler), 'CXX': str(compiler)}), \
                patch.object(toolchain_inputs, 'unpack', side_effect=unpack), \
                patch.object(toolchain_inputs, 'run', side_effect=run):
            environment = toolchain_inputs.prepare_windows(profile, prefix, self.root, lambda entry: wheel, {})
        self.assertEqual((prefix / 'bin/pkg-config.exe').read_bytes(), b'compiled fixture')
        pip = next(command for command in commands if 'pip' in command)
        self.assertTrue({'--no-index', '--no-deps', '--no-cache-dir'}.issubset(pip))
        setup = next(command for command in commands if 'setup' in command)
        self.assertIn('-Ddefault_library=static', setup)
        self.assertIn('-Dtests=disabled', setup)
        self.assertTrue(all('vs_BuildTools.exe' not in item for command in commands for item in command))
        self.assertEqual(environment['GOROOT'], str(prefix))


if __name__ == '__main__':
    unittest.main()
