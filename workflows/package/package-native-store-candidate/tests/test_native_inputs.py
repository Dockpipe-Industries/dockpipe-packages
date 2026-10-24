"""Native upstream recipes preserve exact publisher/ABI and extraction boundaries."""
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import test_native_store as fixtures
import native_inputs
import native_store
import prepare_ci


class NativeInputsTest(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.NativeStoreTest(methodName='runTest')
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root = self.fixture.root
        self.recipes = json.loads((Path(__file__).resolve().parents[4] / 'release/native-input-recipes.json').read_bytes())

    def test_both_actual_upstream_profiles_have_complete_bounded_ownership(self):
        for target, recipe in self.recipes.items():
            with self.subTest(target=target):
                native_inputs.validate(recipe, target)
        self.assertEqual(len(self.recipes['linux-amd64']['archives']), 61)
        self.assertEqual(len(self.recipes['windows-amd64']['archives']), 30)

    def test_linux_qt_opengl_build_inputs_are_exact_and_owned(self):
        recipe = self.recipes['linux-amd64']
        packages = {entry['package']: entry for entry in recipe['archives']}
        for package in ('libgl-dev', 'libglx-dev', 'libopengl-dev', 'libgl1',
                        'libglvnd0', 'libglx0', 'libopengl0', 'libegl1'):
            entry = packages[package + ':amd64']
            self.assertEqual(entry['version'], '1.4.0-1')
            self.assertEqual(entry['destination'], 'gio')
            self.assertTrue(entry['asset']['url'].startswith(
                'https://archive.ubuntu.com/ubuntu/pool/main/libg/libglvnd/'))
            licenses = [record for record in recipe['licenses']
                        if record['archive_id'] == entry['id']]
            self.assertEqual(len(licenses), 1)
        libraries = {Path(record['path']).name for record in recipe['libraries']}
        self.assertTrue({'libGL.so.1', 'libGLX.so.0', 'libOpenGL.so.0',
                         'libGLdispatch.so.0', 'libEGL.so.1'}.issubset(libraries))

    def test_linux_qt_xcb_dependencies_have_exact_archives_libraries_and_licenses(self):
        recipe = self.recipes['linux-amd64']
        expected = {
            'libbsd0': 'libbsd.so.0',
            'libmd0': 'libmd.so.0',
            'libxau6': 'libXau.so.6',
            'libxcb-cursor0': 'libxcb-cursor.so.0',
            'libxcb-icccm4': 'libxcb-icccm.so.4',
            'libxcb-image0': 'libxcb-image.so.0',
            'libxcb-keysyms1': 'libxcb-keysyms.so.1',
            'libxcb-render-util0': 'libxcb-render-util.so.0',
            'libxcb-render0': 'libxcb-render.so.0',
            'libxcb-shape0': 'libxcb-shape.so.0',
            'libxcb-shm0': 'libxcb-shm.so.0',
            'libxcb-util1': 'libxcb-util.so.1',
            'libxcb-xkb1': 'libxcb-xkb.so.1',
            'libxcb1': 'libxcb.so.1',
            'libxdmcp6': 'libXdmcp.so.6',
            'libxkbcommon-x11-0': 'libxkbcommon-x11.so.0',
            'libxkbcommon0': 'libxkbcommon.so.0',
        }
        archives = {entry['package'].split(':')[0]: entry for entry in recipe['archives']}
        for package, soname in expected.items():
            with self.subTest(package=package):
                entry = archives[package]
                self.assertEqual(entry['destination'], 'gio')
                self.assertTrue(entry['asset']['url'].startswith(
                    'https://archive.ubuntu.com/ubuntu/pool/'))
                libraries = [record for record in recipe['libraries']
                             if record['archive_id'] == entry['id']]
                self.assertEqual([Path(record['path']).name for record in libraries], [soname])
                licenses = [record for record in recipe['licenses']
                            if record['archive_id'] == entry['id']]
                self.assertEqual(len(licenses), 1)
        self.assertIn('xkb-data', archives)

    def test_linux_gio_pcre2_metadata_alias_targets_are_exact_and_owned(self):
        recipe = self.recipes['linux-amd64']
        expected = {
            'libpcre2-16-0': 'libpcre2-16.so.0',
            'libpcre2-32-0': 'libpcre2-32.so.0',
            'libpcre2-posix3': 'libpcre2-posix.so.3',
        }
        archives = {entry['package'].split(':')[0]: entry for entry in recipe['archives']}
        for package, soname in expected.items():
            with self.subTest(package=package):
                entry = archives[package]
                self.assertEqual(entry['destination'], 'gio')
                self.assertTrue(entry['asset']['url'].startswith(
                    'https://archive.ubuntu.com/ubuntu/pool/'))
                libraries = [record for record in recipe['libraries']
                             if record['archive_id'] == entry['id']]
                self.assertEqual([Path(record['path']).name for record in libraries], [soname])
                licenses = [record for record in recipe['licenses']
                            if record['archive_id'] == entry['id']]
                self.assertEqual(len(licenses), 1)
        self.assertIn('libpcre2-dev', archives)
        runtime_version = archives['libpcre2-8-0']['version']
        for package in (*expected, 'libpcre2-dev'):
            self.assertEqual(archives[package]['version'], runtime_version)

    def test_recipe_replaces_four_roles_without_loosening_other_public_pins(self):
        lock = self.fixture.lock()
        profile = lock['profiles']['linux-amd64']
        for role in native_inputs.DERIVED_ROLES:
            del profile['inputs'][role]
        profile['native_inputs'] = self.recipes['linux-amd64']
        native_store.load_lock(self.fixture.save_lock(lock), 'linux-amd64')
        profile['inputs']['qt'] = self.fixture.asset()
        with self.assertRaises(ValueError):
            native_store.load_lock(self.fixture.save_lock(lock), 'linux-amd64')

    def test_recipe_matrix_uses_qualified_ubuntu22_and_preserves_legacy_runner(self):
        lock = self.fixture.lock()
        lock['source_commit'] = None
        profile = lock['profiles']['linux-amd64']
        profile['inputs']['source'] = dict(url=None, sha256=None, size_bytes=None, format=None, root=None)
        for role in native_inputs.DERIVED_ROLES:
            del profile['inputs'][role]
        profile['native_inputs'] = self.recipes['linux-amd64']
        def downloader(url, output):
            output.write_bytes(b'committed source transport')
        matrix = prepare_ci.prepare(self.fixture.save_lock(lock), self.root / 'prepared',
            {'GITHUB_REPOSITORY': native_store.REPOSITORY, 'GITHUB_SHA': self.fixture.commit}, downloader)
        self.assertEqual(matrix['include'][0]['runner'], 'ubuntu-22.04')
        self.assertEqual(prepare_ci.RUNNERS['linux-amd64'], 'ubuntu-24.04')

    def test_wrong_abi_foreign_urls_missing_license_owner_and_escapes_rejected(self):
        for mutation in ('compiler', 'url', 'owner', 'path', 'duplicate', 'size'):
            with self.subTest(mutation=mutation):
                recipe = copy.deepcopy(self.recipes['linux-amd64'])
                if mutation == 'compiler':
                    recipe['compiler']['version'] = '13.3.0'
                elif mutation == 'url':
                    recipe['archives'][0]['asset']['url'] = 'https://example.invalid/qt.7z'
                elif mutation == 'owner':
                    recipe['licenses'][0]['archive_id'] = 'not-admitted'
                elif mutation == 'path':
                    recipe['licenses'][0]['path'] = '../host-file'
                elif mutation == 'duplicate':
                    recipe['archives'].append(copy.deepcopy(recipe['archives'][0]))
                else:
                    recipe['archives'][0]['asset']['size_bytes'] = True
                with self.assertRaises(ValueError):
                    native_inputs.validate(recipe, 'linux-amd64')

    def entry(self):
        return {'destination': 'gio', 'asset': {'root': '', 'format': 'deb'}}

    def tar(self, members):
        result = io.BytesIO()
        with tarfile.open(fileobj=result, mode='w') as archive:
            for name, data, link in members:
                item = tarfile.TarInfo(name)
                if link is not None:
                    item.type = tarfile.SYMTYPE
                    item.linkname = link
                    archive.addfile(item)
                else:
                    item.size = len(data)
                    item.mode = 0o755
                    archive.addfile(item, io.BytesIO(data))
        result.seek(0)
        return result

    def test_regular_tar_aliases_flatten_without_host_filesystem_links(self):
        writer = native_inputs.PrefixWriter(self.root)
        native_inputs.tar_members(self.entry(), self.tar([
            ('usr/lib/libtest.so.1.0', b'binary', None),
            ('usr/lib/libtest.so.1', b'', 'libtest.so.1.0')]), writer)
        writer.finish_aliases()
        path = self.root / 'gio/usr/lib/libtest.so.1'
        self.assertFalse(path.is_symlink())
        self.assertEqual(path.read_bytes(), b'binary')
        self.assertTrue(path.stat().st_mode & 0o111)

    def test_escape_alias_cycle_case_collision_device_and_huge_pax_fail_closed(self):
        cases = [[('../outside', b'x', None)], [('A', b'a', None), ('a', b'a', None)],
                 [('x', b'', '../../outside')], [('x', b'', 'y'), ('y', b'', 'x')]]
        for members in cases:
            with self.subTest(members=members):
                writer = native_inputs.PrefixWriter(self.root)
                with self.assertRaises(ValueError):
                    native_inputs.tar_members(self.entry(), self.tar(members), writer)
                    writer.finish_aliases()
        header = tarfile.TarInfo('pax')
        header.type = tarfile.XHDTYPE
        header.size = 65537
        with self.assertRaises(ValueError):
            native_inputs.tar_members(self.entry(), io.BytesIO(header.tobuf()), native_inputs.PrefixWriter(self.root))

    def test_expansion_and_conflicting_archive_overlap_are_rejected(self):
        writer = native_inputs.PrefixWriter(self.root)
        writer.write('gio/file', 3, 0o644, io.BytesIO(b'abc'))
        writer.write('gio/file', 3, 0o644, io.BytesIO(b'abc'))
        with self.assertRaises(ValueError):
            writer.write('gio/file', 3, 0o644, io.BytesIO(b'def'))
        with self.assertRaises(ValueError):
            writer.reserve('gio/huge', native_inputs.MAX_FILE + 1)
        with patch.object(native_inputs, 'MAX_FILES', 1):
            with self.assertRaises(ValueError):
                writer.reserve('gio/many', 0)

    def test_unsupported_compiler_baseline_refused_before_build(self):
        recipe = self.recipes['linux-amd64']
        with patch.object(native_inputs.platform, 'freedesktop_os_release', return_value={'ID':'ubuntu','VERSION_ID':'24.04'}), \
                patch.object(native_inputs.subprocess, 'check_output') as command:
            with self.assertRaises(ValueError):
                native_inputs.compiler_environment(recipe, self.root, {})
            command.assert_not_called()

    def test_linux_case_sensitive_docs_and_merged_usr_are_explicit(self):
        writer = native_inputs.PrefixWriter(self.root, case_sensitive=True)
        native_inputs.tar_members(self.entry(), self.tar([
            ('lib/tool', b'library', None),
            ('usr/share/doc/Index.html', b'upper', None),
            ('usr/share/doc/index.html', b'lower', None)]), writer)
        self.assertEqual((self.root / 'gio/usr/lib/tool').read_bytes(), b'library')
        self.assertEqual((self.root / 'gio/usr/share/doc/Index.html').read_bytes(), b'upper')
        self.assertEqual((self.root / 'gio/usr/share/doc/index.html').read_bytes(), b'lower')

    def test_hashed_hosted_descriptor_keeps_source_bytes_on_windows_checkout(self):
        source = Path(__file__).resolve().parents[4]
        attributes = (source / '.gitattributes').read_text().splitlines()
        self.assertIn('release/native-compiler/windows-hosted-19.51.json text eol=lf', attributes)
        descriptor = source / 'release/native-compiler/windows-hosted-19.51.json'
        expected = self.recipes['windows-amd64']['compiler']['acquisition_sha256']
        self.assertEqual(native_inputs.digest(descriptor), expected)
        self.assertNotEqual(hashlib.sha256(descriptor.read_bytes().replace(b'\n', b'\r\n')).hexdigest(), expected)

    @patch('windows_runtime.validate_runtime', return_value=Path('/qualified-runtime'))
    def test_native_msvc_receipt_is_admitted_without_redistributing_compiler(self, runtime_admission):
        from types import SimpleNamespace
        installation = self.root / 'ProgramFiles/Microsoft Visual Studio/18/Enterprise'
        directory = installation / 'VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64'
        directory.mkdir(parents=True)
        tools = {}
        for name in ('cl', 'dumpbin', 'lib', 'link'):
            path = directory / (name + '.exe')
            path.write_bytes(('synthetic ' + name).encode())
            tools[name] = {'path':path.relative_to(installation).as_posix(),
                           'sha256':native_inputs.digest(path)}
        acquisition = json.loads((Path(__file__).resolve().parents[4] /
                                  'release/native-compiler/windows-hosted-19.51.json').read_bytes())
        # This orchestration fixture supplies synthetic tools; exact real compiler bytes
        # are admitted independently against the source descriptor before execution.
        acquisition['compiler_payload']['compiler_sha256'] = tools['cl']['sha256']
        original_read_bytes = Path.read_bytes
        def read_fixture(path):
            if path.name == 'windows-hosted-19.51.json':
                return json.dumps(acquisition).encode()
            return original_read_bytes(path)
        self.enterContext(patch.object(Path, 'read_bytes', read_fixture))
        recipe = self.recipes['windows-amd64']
        receipt = {'schema':1,'compiler_id':'MSVC','compiler_version':'19.51.36260.0',
                   'windows_sdk_version':'10.0.26100.0',
                   'acquisition_sha256':recipe['compiler']['acquisition_sha256'],
                   'acquisition_mode':'hosted-exact', 'installation':str(installation),
                   'observed_image':{'image_os':'observed', 'image_version':'observed'},
                   'tools':tools, 'host_runtime':{}}
        path = self.root / 'compiler.json'
        path.write_text(json.dumps(receipt))
        sdk = self.root / 'ProgramFilesX86/Windows Kits/10'
        sdk_bin = sdk / 'bin/10.0.26100.0/x64'
        sdk_bin.mkdir(parents=True)
        for name in ('rc', 'mt'):
            (sdk_bin / (name + '.exe')).write_bytes(b'signed installer fixture')
        receipt['sdk_tools'] = {name: {'path': 'bin/10.0.26100.0/x64/' + name + '.exe',
                                     'sha256': native_inputs.digest(sdk_bin / (name + '.exe'))}
                                for name in ('rc', 'mt')}
        path.write_text(json.dumps(receipt))
        environment = {'GITHUB_ACTIONS':'true', 'RUNNER_ENVIRONMENT':'github-hosted',
                       'PROGRAMFILES':str(self.root / 'ProgramFiles'),
                       'PROGRAMFILES(X86)':str(self.root / 'ProgramFilesX86'),
                       'VCTOOLSINSTALLDIR':str(installation / 'VC/Tools/MSVC/14.51.36231'),
                       'VCTOOLSVERSION':'14.51.36231', 'WINDOWSSDKDIR': str(sdk), 'WINDOWSSDKVERSION': '10.0.26100.0/',
                       'DOCKPIPE_NATIVE_MSVC_ROOT':str(installation),
                       'DOCKPIPE_NATIVE_MSVC_RECEIPT':str(path), 'INCLUDE':'declared include',
                       'LIB':'declared library', 'GH_TOKEN':'must not reach compiler'}
        with patch.object(native_inputs.subprocess, 'run', return_value=SimpleNamespace(
                stdout='Compiler Version 19.51.36260 for x64',stderr='')):
            result = native_inputs.compiler_environment(recipe, self.root / 'tools', environment)
        original_acquisition = receipt['acquisition_sha256']
        descriptor_bytes = (Path(__file__).resolve().parents[4] /
                            'release/native-compiler/windows-hosted-19.51.json').read_bytes()
        receipt['acquisition_sha256'] = hashlib.sha256(descriptor_bytes.replace(b'\n', b'\r\n')).hexdigest()
        path.write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, 'source-bound hosted ABI'):
            native_inputs.compiler_environment(recipe, self.root / 'tools', environment)
        receipt['acquisition_sha256'] = original_acquisition
        path.write_text(json.dumps(receipt))
        self.assertEqual(result['CC'], str(directory / 'cl.exe'))
        self.assertEqual(result['CMAKE_GENERATOR'], 'Ninja')
        self.assertEqual(result['DOCKPIPE_VM_DISPLAY_INSPECTOR'], str(directory / 'dumpbin.exe'))
        self.assertNotIn('GH_TOKEN', result)
        self.assertEqual(result['DOCKPIPE_NATIVE_RC'], str(sdk_bin / 'rc.exe'))
        self.assertIn(str(sdk_bin), result['PATH'])
        incomplete = dict(environment)
        del incomplete['WINDOWSSDKDIR']
        with self.assertRaisesRegex(ValueError, 'Windows SDK environment'):
            native_inputs.compiler_environment(recipe, self.root / 'tools', incomplete)
        for field, value in [('acquisition_sha256','b'*64),('compiler_version','19.45.0'),
                             ('acquisition_mode','installer')]:
            changed = copy.deepcopy(receipt)
            changed[field] = value
            path.write_text(json.dumps(changed))
            with self.subTest(field=field), patch.object(native_inputs.subprocess,'run') as command:
                with self.assertRaises(ValueError):
                    native_inputs.compiler_environment(recipe,self.root / 'tools',environment)
                command.assert_not_called()
        path.write_text(json.dumps(receipt))
        for field, value in [('PROGRAMFILES', str(self.root / 'other')),
                             ('VCTOOLSVERSION', '14.44.35207'),
                             ('VCTOOLSINSTALLDIR', str(self.root / 'old-tools')),
                             ('RUNNER_ENVIRONMENT', 'self-hosted'), ('GITHUB_ACTIONS', 'false')]:
            changed_environment = dict(environment, **{field: value})
            with self.subTest(environment=field), patch.object(native_inputs.subprocess, 'run') as command:
                with self.assertRaises(ValueError):
                    native_inputs.compiler_environment(recipe, self.root / 'tools', changed_environment)
                command.assert_not_called()
        changed = copy.deepcopy(receipt)
        changed['sdk_tools']['rc']['sha256'] = 'f' * 64
        path.write_text(json.dumps(changed))
        with patch.object(native_inputs.subprocess, 'run') as command:
            with self.assertRaisesRegex(ValueError, 'SDK resource/manifest'):
                native_inputs.compiler_environment(recipe, self.root / 'tools', environment)
            command.assert_not_called()
        receipt['tools']['cl']['path'] = '../outside'
        path.write_text(json.dumps(receipt))
        with self.assertRaises(ValueError):
            native_inputs.compiler_environment(recipe,self.root / 'tools',environment)

    def test_hosted_compiler_mode_and_exact_baseline_are_mandatory(self):
        for field, value in [('acquisition_mode', 'installer'), ('acquisition_mode', None),
                             ('version', '19.44.35207.1'), ('os_version', 'native')]:
            recipe = copy.deepcopy(self.recipes['windows-amd64'])
            if value is None:
                del recipe['compiler'][field]
            else:
                recipe['compiler'][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                native_inputs.validate(recipe, 'windows-amd64')

    def test_blocked_installer_assets_are_not_published_with_hosted_profile(self):
        source = Path(__file__).resolve().parents[4]
        files = {path.name for path in (source / 'release/native-compiler').iterdir()}
        self.assertEqual(files, {'windows-hosted-19.51.json'})
        descriptor = json.loads((source / 'release/native-compiler/windows-hosted-19.51.json').read_bytes())
        self.assertEqual(descriptor['display_sdk']['source_sha'], '79bb110c712e0d71d8bcde4cecff725f3c107a49')
        self.assertEqual(descriptor['display_sdk']['compiler_version'], '19.51.36260.0')
        self.assertEqual(descriptor['compiler_payload']['compiler_sha256'],
                         'b0a1818ebae6da7f18230a84e76ce839212584ac4c2ab55b97a0d5268085da57')
        self.assertFalse(descriptor['policy']['installer_execution'])
        self.assertFalse(descriptor['policy']['system_installation'])
        self.assertFalse(descriptor['policy']['fallback'])

    def test_hosted_metadata_is_source_bound_without_installer_or_channel(self):
        source = Path(__file__).resolve().parents[4]
        descriptor_path = source / 'release/native-compiler/windows-hosted-19.51.json'
        descriptor = json.loads(descriptor_path.read_bytes())
        self.assertEqual(native_inputs.digest(descriptor_path),
                         self.recipes['windows-amd64']['compiler']['acquisition_sha256'])
        self.assertEqual(descriptor['acquisition_mode'], 'hosted-exact')
        self.assertEqual(descriptor['runner_label'], 'windows-2025-vs2026')
        self.assertFalse(descriptor['observed_image']['immutable_image_pin'])
        self.assertNotIn('channel', descriptor)
        self.assertNotIn('bootstrapper', descriptor)
        script = (fixtures.SCRIPTS / 'prepare_msvc.ps1').read_text()
        self.assertIn("$env:RUNNER_ENVIRONMENT -ne 'github-hosted'", script)
        self.assertIn('Get-AuthenticodeSignature', script)
        self.assertIn('10.0.26100.0 -vcvars_ver=14.51.36231', script)
        self.assertIn("@('cl', 'dumpbin', 'lib', 'link')", script)
        self.assertIn("@('rc', 'mt')", script)
        self.assertIn('ReparsePoint', script)
        self.assertIn("$_ -like '__VSCMD_*'", script)
        self.assertIn('sdk_tools = $sdkTools', script)
        for forbidden in ('Start-Process', 'vs_BuildTools.exe', '--channelUri', 'curl.exe', 'aka.ms'):
            self.assertNotIn(forbidden, script)
        workflow = (source / '.github/workflows/native-store-candidate.yml').read_text()
        self.assertEqual(prepare_ci.runner_for('windows-amd64', {'native_inputs': self.recipes['windows-amd64']}),
                         'windows-2025-vs2026')
        self.assertEqual(prepare_ci.runner_for('windows-amd64', {}), 'windows-2022')
        self.assertNotIn('continue-on-error', workflow)

    def test_qt_factory_writes_only_bounded_regular_material(self):
        import py7zr
        source = self.root / 'input'
        source.write_bytes(b'qt fixture')
        archive = self.root / 'qt.7z'
        with py7zr.SevenZipFile(archive, 'w') as bundle:
            bundle.write(source, 'bin/tool')
        entry = {'destination':'qt', 'asset':{'root':'','format':'7z'}}
        native_inputs.sevenzip_members(entry, archive, native_inputs.PrefixWriter(self.root))
        self.assertEqual((self.root / 'qt/bin/tool').read_bytes(), b'qt fixture')


if __name__ == '__main__':
    unittest.main()
