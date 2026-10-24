import os
import json
from pathlib import Path
import shutil
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[1] / 'assets/scripts'
sys.path.insert(0, str(SCRIPTS))
import sdk_pkgconfig
import native_inputs_macos


@unittest.skipIf(os.name == 'nt', 'Apple SDK metadata uses POSIX paths')
class SdkPkgconfigTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.sdk = self.root / 'MacOSX15.5 SDK.sdk'
        self.tool = shutil.which('pkg-config')
        if not self.tool:
            self.fail('These graph regressions require an actual pkg-config executable')
        for module, files in sdk_pkgconfig.MODULES.items():
            for relative in files:
                path = self.sdk / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'synthetic SDK fixture; not native qualification\n')
            directory = self.sdk / 'usr/lib/pkgconfig'
            directory.mkdir(exist_ok=True)
            content = ('prefix=/usr\nlibdir=${prefix}/lib\nincludedir=${prefix}/include' + ('/ffi' if module == 'libffi' else '')
                       + '\nName: ' + module + '\nDescription: SDK fixture\nVersion: 1.2.3\nLibs: -L${libdir} -l'
                       + ('ffi' if module == 'libffi' else 'z') + '\nCflags: -I${includedir}\n')
            (directory / (module + '.pc')).write_text(content)

    def project(self):
        return sdk_pkgconfig.project(self.sdk, '15.5', self.root / 'private-pc', self.tool, os.environ)

    def test_projects_exact_sdk_files_and_preserves_private_paths(self):
        original = {str(path): path.read_bytes() for path in self.sdk.rglob('*') if path.is_file()}
        receipt = self.project()
        self.assertEqual(set(receipt['modules']), {'libffi', 'zlib'})
        for record in receipt['modules'].values():
            self.assertEqual(record['version'], '1.2.3')
            self.assertEqual(len(record['source_files']), 4)
        self.assertEqual(original, {str(path): path.read_bytes() for path in self.sdk.rglob('*') if path.is_file()})
        for path in (self.root / 'private-pc').iterdir():
            self.assertIn(str(self.sdk).replace(' ', '\\ '), path.read_text())
        with self.assertRaisesRegex(ValueError, 'fresh private'):
            self.project()

    def test_missing_or_escaping_sdk_file_fails_with_expected_path(self):
        path = self.sdk / 'usr/include/zlib.h'
        path.unlink()
        with self.assertRaisesRegex(ValueError, 'usr/include/zlib.h'):
            self.project()
        outside = self.root / 'outside.h'
        outside.write_bytes(b'not SDK owned')
        path.symlink_to(outside)
        with self.assertRaisesRegex(ValueError, 'escapes its root'):
            self.project()

    def test_malformed_metadata_and_ambient_prefix_are_rejected(self):
        path = self.sdk / 'usr/lib/pkgconfig/zlib.pc'
        original = path.read_text()
        outside = self.root / 'outside'
        outside.mkdir()
        for content in ('Name: broken\n', original.replace('prefix=/usr', 'prefix=/opt/homebrew'),
                        original.replace('Cflags: -I${includedir}', 'Cflags: -I' + str(outside))):
            with self.subTest(content=content):
                path.write_text(content)
                target = self.root / ('projection-' + str(len(list(self.root.iterdir()))))
                with self.assertRaises(ValueError):
                    sdk_pkgconfig.project(self.sdk, '15.5', target, self.tool, os.environ)
        path.write_bytes(b'x' * (sdk_pkgconfig.MAX_METADATA_BYTES + 1))
        with self.assertRaisesRegex(ValueError, 'exceeds its bound'):
            self.project()

    def test_real_private_pkgconfig_graph_fails_before_projection_then_closes(self):
        gio = self.root / 'gio/lib/pkgconfig'
        gio.mkdir(parents=True)
        (gio / 'gio-unix-2.0.pc').write_text(
            'prefix=' + str(self.root / 'gio') + '\nName: gio-unix\nDescription: fixture\nVersion: 2.90.1\n'
            'Requires.private: libffi >= 1.0, zlib >= 1.0\nCflags: -I${prefix}/include\nLibs: -L${prefix}/lib -lgio-2.0\n')
        environment = dict(os.environ, PKG_CONFIG_PATH='', PKG_CONFIG_SYSROOT_DIR='', PKG_CONFIG_LIBDIR=str(gio))
        with self.assertRaisesRegex(ValueError, 'graph failed'):
            sdk_pkgconfig.admit_gio_graph(self.tool, environment)
        receipt = self.project()
        environment['PKG_CONFIG_LIBDIR'] = os.pathsep.join((str(gio), receipt['directory']))
        self.assertEqual(sdk_pkgconfig.admit_gio_graph(self.tool, environment)['version'], '2.90.1')
        flags = subprocess.check_output([self.tool, '--cflags', '--libs', 'gio-unix-2.0'], env=environment, text=True)
        self.assertTrue(any(flag.startswith('-I' + str(self.sdk)) for flag in shlex.split(flags)))
        self.assertIn(str(self.root / 'gio'), flags)
        self.assertEqual(environment['PKG_CONFIG_SYSROOT_DIR'], '')

    def test_sdk_version_and_absolute_root_are_required(self):
        for root, version in ((self.sdk, '26.0'), (Path('relative.sdk'), '15.5')):
            with self.subTest(root=root, version=version), self.assertRaisesRegex(ValueError, 'admitted SDK15.5'):
                sdk_pkgconfig.project(root, version, self.root / 'pc', self.tool, os.environ)

    def test_prepare_reports_missing_sdk_metadata_before_download_or_broker_build(self):
        (self.sdk / 'usr/lib/pkgconfig/zlib.pc').unlink()
        toolchain = self.root / 'toolchain'
        (toolchain / 'bin').mkdir(parents=True)
        for name in ('cmake', 'pkg-config'):
            (toolchain / 'bin' / name).write_bytes(b'not executed in this failure test')
        recipe_path = Path(__file__).resolve().parents[4] / 'release/native-inputs/darwin-arm64.json'
        recipe = json.loads(recipe_path.read_text())
        acquire = mock.Mock()
        with mock.patch.object(native_inputs_macos.platform, 'system', return_value='Darwin'), \
                mock.patch.object(native_inputs_macos.platform, 'machine', return_value='arm64'), \
                mock.patch.object(native_inputs_macos.platform, 'mac_ver', return_value=('15.7.9', (), '')), \
                mock.patch.object(native_inputs_macos, 'run', side_effect=[str(self.sdk), '15.5']) as run:
            with self.assertRaisesRegex(ValueError, 'usr/lib/pkgconfig/zlib.pc'):
                native_inputs_macos.prepare(recipe, self.root / 'output', toolchain,
                                           self.root / 'cache', False, acquire, {})
        acquire.assert_not_called()
        self.assertEqual(run.call_count, 2)

    def test_direct_sdk_usr_flags_are_projected_without_a_global_sysroot(self):
        path = self.sdk / 'usr/lib/pkgconfig/zlib.pc'
        path.write_text(path.read_text().replace('Cflags: -I${includedir}', 'Cflags: -I/usr/include'))
        receipt = self.project()
        self.assertEqual(receipt['modules']['zlib']['version'], '1.2.3')
