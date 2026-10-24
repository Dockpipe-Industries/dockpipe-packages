"""Source-bound Mac build-tool admission and orchestration; no native execution claim."""
import copy
import hashlib
import json
from pathlib import Path
import unittest
from unittest.mock import patch

import test_native_store as fixtures
import framework_extract
import native_inputs_macos
import native_store
import toolchain_inputs


ROOT = Path(__file__).resolve().parents[4]


class MacToolchainTest(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.NativeStoreTest(methodName='runTest')
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root = self.fixture.root
        self.profile = json.loads((ROOT / 'release/native-toolchain/darwin-arm64.json').read_text())
        self.path = self.root / 'mac-profile.json'
        self.path.write_text(json.dumps(self.profile))
        self.reference = {'path': self.path.name, 'sha256': hashlib.sha256(self.path.read_bytes()).hexdigest()}

    def test_actual_profile_contains_only_exact_official_tools_and_native_compiler_baseline(self):
        toolchain_inputs.validate_profile(self.profile, 'darwin-arm64')
        self.assertEqual(self.profile['compiler'], native_inputs_macos.BASELINE)
        self.assertEqual({entry['id'] for entry in self.profile['inputs']}, {'go', 'cmake', 'ninja', 'pkgconf-source'})
        self.assertEqual(self.profile['compiler_inputs'], [])
        self.assertEqual(self.profile['inputs'][0]['sha256'], '0b5183492292ada170f9b561a834863808dd02edba599e4ad0e89899330020b3')
        self.assertEqual(self.profile['inputs'][1]['sha256'], '330b9514f5112e5ed4fb08b8b05803b776fd9b539a6ae12927d14dcc0ee2ba8d')

    def test_newer_os_or_wrong_architecture_cannot_be_relabelled_mac15_release_proof(self):
        for version, architecture in [('27.0', 'arm64'), ('15.5', 'x86_64')]:
            with self.subTest(version=version, architecture=architecture):
                with patch.object(toolchain_inputs.platform, 'system', return_value='Darwin'), \
                        patch.object(toolchain_inputs.platform, 'mac_ver', return_value=(version, (), architecture)), \
                        patch.object(toolchain_inputs.platform, 'machine', return_value=architecture), \
                        patch.object(toolchain_inputs.toolchain_acquire, 'acquire') as acquire:
                    with self.assertRaisesRegex(ValueError, 'arm64/macOS15'):
                        toolchain_inputs.prepare(self.root, self.reference, 'darwin-arm64', self.root / 'prefix',
                                                 self.root / 'cache', False, {})
                    acquire.assert_not_called()

    def test_darwin_lock_uses_recipe_instead_of_unresolved_direct_toolchain_bundle(self):
        document = self.fixture.lock()
        profile = document['profiles'].pop('linux-amd64')
        profile['native_inputs'] = json.loads((ROOT / 'release/native-inputs/darwin-arm64.json').read_text())
        for role in ('qt', 'gio', 'broker', 'broker_provenance', 'toolchain'):
            del profile['inputs'][role]
        profile['toolchain_recipe'] = self.reference
        document['accepted_platforms'] = ['darwin-arm64']
        document['profiles'] = {'darwin-arm64': profile}
        native_store.load_lock(self.fixture.save_lock(document), 'darwin-arm64')
        profile['inputs']['toolchain'] = self.fixture.asset()
        with self.assertRaises(ValueError):
            native_store.load_lock(self.fixture.save_lock(document), 'darwin-arm64')
        example = json.loads((ROOT / 'release/native-store.lock.example.json').read_text())
        self.assertEqual(len(example['owned']), 8)
        self.assertNotIn('toolchain', example['profiles']['darwin-arm64']['inputs'])
        self.assertEqual(set(example['accepted_platforms']), set(example['profiles']))
        self.assertIn('darwin-arm64', example['accepted_platforms'])
        self.assertNotIn('linux-arm64', native_store.SUPPORTED)

    def test_mac_requires_job_local_xcode_and_exact_sdk_before_building(self):
        with self.assertRaisesRegex(ValueError, 'job-local Xcode16.4'):
            toolchain_inputs.prepare_macos(self.profile, self.root / 'prefix', lambda entry: self.path, {})
        with patch.object(Path, 'is_dir', return_value=True), \
                patch.object(toolchain_inputs, 'run', side_effect=['/selected/sdk', '26.0']):
            with self.assertRaisesRegex(ValueError, 'SDK15.5'):
                toolchain_inputs.prepare_macos(self.profile, self.root / 'prefix', lambda entry: self.path,
                    {'DEVELOPER_DIR': '/Applications/Xcode_16.4.app/Contents/Developer'})
        self.assertFalse((self.root / 'prefix').exists())

    def test_mac_dispatch_retains_framework_aliases_and_builds_pkgconf_with_selected_clang(self):
        prefix = self.root / 'prefix'
        developer = Path('/Applications/Xcode_16.4.app/Contents/Developer')
        sdk = self.root / 'SDK with spaces'
        sdk.mkdir()
        commands = []
        original_is_dir = Path.is_dir
        original_is_file = Path.is_file
        def is_dir(path):
            return path == developer or original_is_dir(path)
        def is_file(path):
            return path.is_relative_to(developer) or original_is_file(path)
        def unpack(entry, archive, destination):
            destination.mkdir(parents=True)
            files = {'go': ['bin/go', 'bin/gofmt'], 'ninja': ['ninja'], 'pkgconf-source': ['configure']}
            for name in files[entry['id']]:
                path = destination / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'official fixture')
        def frameworks(records, destination):
            root = destination / self.profile['inputs'][1]['root'] / 'CMake.app/Contents/bin'
            root.mkdir(parents=True)
            for name in ('cmake', 'ctest'):
                (root / name).write_bytes(b'official fixture')
        def run(arguments, environment, directory=None):
            commands.append(([str(item) for item in arguments], dict(environment)))
            if arguments[:3] == ['/usr/bin/xcrun', '--sdk', 'macosx']:
                return '15.5' if arguments[-1] == '--show-sdk-version' else str(sdk)
            if arguments[:2] == ['/usr/bin/xcrun', '--find']:
                return str(developer / 'Toolchains/XcodeDefault.xctoolchain/usr/bin' / arguments[-1])
            if arguments == ['/usr/bin/make', 'install']:
                path = prefix / 'pkgconf/bin/pkgconf'
                path.parent.mkdir(parents=True)
                path.write_bytes(b'compiled fixture')
            return ''
        with patch.object(Path, 'is_dir', is_dir), patch.object(Path, 'is_file', is_file), \
                patch.object(toolchain_inputs, 'unpack', side_effect=unpack), \
                patch.object(framework_extract, 'extract_locked', side_effect=frameworks), \
                patch.object(toolchain_inputs, 'run', side_effect=run):
            environment = toolchain_inputs.prepare_macos(self.profile, prefix, lambda entry: self.path,
                {'DEVELOPER_DIR': str(developer)})
        configure = next(item for item in commands if item[0][0].endswith('/configure'))
        self.assertEqual(configure[1]['MACOSX_DEPLOYMENT_TARGET'], '15.0')
        self.assertIn("'" + str(sdk) + "'", configure[1]['CFLAGS'])
        self.assertIn('--disable-shared', configure[0])
        self.assertTrue(environment['CXX'].startswith(str(developer)))
        self.assertEqual((prefix / 'bin/pkg-config').resolve().read_bytes(), b'compiled fixture')
        self.assertTrue((prefix / 'bin/cmake').resolve().is_relative_to(prefix))
        self.assertFalse(any('xcode-select' in argument for command, _ in commands for argument in command))

    def test_receipt_requires_native_compiler_sdk_and_license_inventory(self):
        license_record = {'path': 'go/LICENSE', 'sha256': 'c' * 64, 'size_bytes': 10}
        receipt = {'schema': 1, 'platform': 'darwin-arm64', 'profile': self.reference,
            'tools': {name: {'version': version, 'sha256': 'd' * 64} for name, version in
                      [('go', 'go1.26.10'), ('cmake', '3.31.6'), ('ninja', '1.12.1'), ('pkg-config', '2.3.0')]},
            'compiler': self.profile['compiler'], 'acquisitions': self.profile['inputs'],
            'inventory': [license_record], 'licenses': [license_record], 'system_installs_performed': False,
            'checks': ['exact-tool-versions', 'native-shared-library-consumer', 'isolated-pkgconf-metadata'],
            'native_sdk': {'xcode_version': '16.4', 'sdk_version': '15.5', 'deployment_target': '15.0',
                           'compilers': {'CC': 'e' * 64, 'CXX': 'f' * 64}}}
        toolchain_inputs.validate_receipt(receipt, 'darwin-arm64', self.reference)
        for key, value in [('sdk_version', '26.0'), ('xcode_version', '27.0'), ('deployment_target', '13.0')]:
            changed = copy.deepcopy(receipt)
            changed['native_sdk'][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                toolchain_inputs.validate_receipt(changed, 'darwin-arm64', self.reference)
        receipt['licenses'][0] = {'path': '../host-license', 'sha256': 'c' * 64, 'size_bytes': 10}
        with self.assertRaises(ValueError):
            toolchain_inputs.validate_receipt(receipt, 'darwin-arm64', self.reference)


if __name__ == '__main__':
    unittest.main()
