"""Prepared artifacts retain exact source, SDK, Qt and native-prefix admission."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
PACKAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE))
specification = importlib.util.spec_from_file_location('display_producer', PACKAGE / 'build.py')
producer = importlib.util.module_from_spec(specification)
specification.loader.exec_module(producer)


class PreparedBuildTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.current = self.root / 'current'
        self.source = self.root / 'prepared-source'
        self.build = self.root / 'build'
        self.sdk = self.root / 'sdk/lib/cmake/DockPipeDisplaySDK'
        self.qt = self.root / 'qt/lib/cmake/Qt6'
        self.gio = self.root / 'gio'
        for directory in (self.current, self.source):
            (directory / 'src').mkdir(parents=True)
            (directory / 'CMakeLists.txt').write_text('project(fixture)\n')
            (directory / 'src/Frame.cpp').write_text('void frame() {}\n')
            (directory / 'src/Frame.h').write_text('#pragma once\n')
            (directory / 'src/display-metadata.json.in').write_text('{}\n')
        for directory in (self.build, self.sdk, self.qt, self.gio):
            directory.mkdir(parents=True, exist_ok=True)
        self.sdk.joinpath('DockPipeDisplaySDKConfig.cmake').write_text(
            'find_dependency(Qt6 6.10.3 EXACT COMPONENTS Widgets)\n'
            'set(DockPipeDisplaySDK_COMPILER_ID "MSVC")\n'
            'set(DockPipeDisplaySDK_COMPILER_VERSION "19.44.35229.0")\n'
            'set(DockPipeDisplaySDK_SYSTEM_NAME "Windows")\n'
            'set(DockPipeDisplaySDK_SYSTEM_PROCESSOR "AMD64")\n')
        self.cache = {
            'CMAKE_HOME_DIRECTORY': str(self.source),
            'CMAKE_BUILD_TYPE': 'Release',
            'DOCKPIPE_BUILD_DISPLAY_PLUGIN': 'ON',
            'DOCKPIPE_DISPLAY_SDK': 'display',
            'DockPipeDisplaySDK_DIR': str(self.sdk),
            'Qt6_DIR': str(self.qt),
            'DOCKPIPE_GIO_PREFIX': str(self.gio),
        }
        self.write_cache()
        self.metadata = {'display_abi': 1, 'qt_version': '6.10.3',
                         'build_abi': 'x86_64-little_endian-llp64',
                         'compiler_id': 'MSVC', 'compiler_version': '19.44.35229.0'}
        self.write_metadata()
        self.root_patch = patch.object(producer, 'ROOT', self.current)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)

    def test_material_inventory_includes_cmake_test_runner(self):
        tooling = self.current / 'tooling'
        tooling.mkdir()
        runner = tooling / 'RunNativeTest.cmake'
        runner.write_text('execute_process(COMMAND fixture)\n')
        inventory = producer.material_sources(self.current)
        self.assertEqual(inventory['tooling/RunNativeTest.cmake'], producer.sha256(runner))

    def write_cache(self):
        self.build.joinpath('CMakeCache.txt').write_text(''.join(
            key + ':STRING=' + value + '\n' for key, value in self.cache.items()))

    def write_metadata(self):
        self.build.joinpath('display-metadata.json').write_text(json.dumps(self.metadata))

    def admit(self):
        return producer.admit_prepared_build(self.build, self.sdk, self.qt,
                                             'DockPipeDisplaySDK', 'display',
                                             'Windows', 'amd64', self.gio)

    def test_matching_material_and_contract_are_admitted(self):
        self.assertEqual(self.admit(), self.build.resolve())
        # Packaging-only Python changes do not invalidate unchanged native code.
        self.current.joinpath('build.py').write_text('new packaging only\n')
        self.assertEqual(self.admit(), self.build.resolve())

    def test_case_insensitive_filesystem_identity_is_used(self):
        self.cache['DockPipeDisplaySDK_DIR'] = str(self.root / 'SDK-case-alias')
        self.write_cache()
        # Native macOS evidence has distinct resolved spellings for one inode.
        with patch.object(Path, 'samefile', return_value=True) as identity:
            self.assertEqual(self.admit(), self.build.resolve())
        identity.assert_any_call(self.sdk)

    def test_changed_missing_or_added_native_source_is_rejected(self):
        path = self.source / 'src/Frame.cpp'
        path.write_text('void changed() {}\n')
        with self.assertRaisesRegex(ValueError, 'material source differs'):
            self.admit()
        path.unlink()
        with self.assertRaisesRegex(ValueError, 'material source differs'):
            self.admit()
        path.write_text('void frame() {}\n')
        self.source.joinpath('src/Added.h').write_text('#pragma once\n')
        with self.assertRaisesRegex(ValueError, 'material source differs'):
            self.admit()

    def test_other_sdk_qt_or_dependency_prefix_is_rejected(self):
        for field in ('DockPipeDisplaySDK_DIR', 'Qt6_DIR', 'DOCKPIPE_GIO_PREFIX'):
            with self.subTest(field=field):
                original = self.cache[field]
                self.cache[field] = str(self.root / 'other')
                self.write_cache()
                with self.assertRaisesRegex(ValueError, 'different'):
                    self.admit()
                self.cache[field] = original

    def test_debug_or_nonplugin_build_is_rejected(self):
        for field, value in (('CMAKE_BUILD_TYPE', 'Debug'),
                             ('DOCKPIPE_BUILD_DISPLAY_PLUGIN', 'OFF'),
                             ('DOCKPIPE_DISPLAY_SDK', 'launcher')):
            with self.subTest(field=field):
                original = self.cache[field]
                self.cache[field] = value
                self.write_cache()
                with self.assertRaisesRegex(ValueError, 'configuration differs'):
                    self.admit()
                self.cache[field] = original

    def test_generated_qt_compiler_and_abi_mismatch_is_rejected(self):
        for field in ('qt_version', 'compiler_version', 'compiler_id', 'build_abi'):
            with self.subTest(field=field):
                original = self.metadata[field]
                self.metadata[field] = 'incompatible'
                self.write_metadata()
                with self.assertRaisesRegex(ValueError, 'metadata differs'):
                    self.admit()
                self.metadata[field] = original

    def test_duplicate_or_oversized_cache_is_rejected(self):
        cache = self.build / 'CMakeCache.txt'
        with cache.open('a') as stream:
            stream.write('CMAKE_HOME_DIRECTORY:STRING=' + str(self.source) + '\n')
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            self.admit()
        cache.write_text('x' * (1024 * 1024 + 1))
        with self.assertRaisesRegex(ValueError, 'out-of-bounds'):
            self.admit()


if __name__ == '__main__':
    unittest.main()
