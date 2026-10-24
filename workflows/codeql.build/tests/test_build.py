"""CodeQL builds authenticate the SDK before module downloads or compilation."""
import importlib.util
import json
import os
import subprocess
from pathlib import Path
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / 'assets/scripts/build.py'
SPEC = importlib.util.spec_from_file_location('catalog_codeql_build', SCRIPT)
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)


class CodeQLBuildTests(unittest.TestCase):
    def test_missing_external_sdk_selection_fails_before_build(self):
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(BUILD.subprocess, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'must select'):
                BUILD.main()
            run.assert_not_called()

    def prepare_repository(self, root):
        (root / 'release').mkdir()
        lock = {'manifest_sha256': 'a' * 64}
        (root / 'release/dependencies.lock.json').write_text(json.dumps({'dockpipe_go_sdk': lock}))
        (root / 'go.mod').write_text('module example.invalid/catalog\n\nrequire dockpipe v0.0.0\n')
        (root / 'go.sum').write_text('')
        return lock

    def test_rejected_sdk_never_downloads_or_builds_modules(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.prepare_repository(root)
            with mock.patch.object(BUILD, 'ROOT', root), \
                    mock.patch.dict(os.environ, {'DOCKPIPE_GO_SDK_INPUTS': str(root / 'inputs')}), \
                    mock.patch.object(BUILD.catalog_go_dependencies.go_candidate, 'unpack_sdk', side_effect=ValueError('invalid SDK digest')), \
                    mock.patch.object(BUILD.subprocess, 'run') as run:
                with self.assertRaisesRegex(ValueError, 'invalid SDK digest'):
                    BUILD.main()
                run.assert_not_called()

    def test_verified_sdk_builds_catalog_and_nested_vm_module(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock = self.prepare_repository(root)
            manifest = {'source_digest': 'b' * 64}

            def unpack(metadata, archive, expected, output):
                self.assertEqual(expected, lock['manifest_sha256'])
                output.mkdir()
                (output / 'go.sum').write_text('')
                return manifest

            with mock.patch.object(BUILD, 'ROOT', root), \
                    mock.patch.dict(os.environ, {'DOCKPIPE_GO_SDK_INPUTS': str(root / 'inputs')}), \
                    mock.patch.object(BUILD.catalog_go_dependencies.go_candidate, 'unpack_sdk', side_effect=unpack), \
                    mock.patch.object(BUILD.catalog_go_dependencies.go_candidate, 'validate_sdk_pin') as validate, \
                    mock.patch.object(BUILD.platform, 'system', return_value='Linux'), \
                    mock.patch.object(BUILD.subprocess, 'run') as run:
                BUILD.main()
            validate.assert_called_once_with(root, manifest, lock['manifest_sha256'])
            calls = run.call_args_list
            self.assertEqual(len(calls), 6)
            self.assertEqual(calls[1].args[0][:3], ['go', 'mod', 'download'])
            self.assertEqual(calls[2].args[0][:3], ['go', 'list', '-mod=mod'])
            self.assertEqual(calls[2].args[0][3], calls[1].args[0][3])
            self.assertEqual(calls[2].args[0][-3:], ['-deps', '-test', './...'])
            self.assertEqual(calls[2].kwargs['stdout'], subprocess.DEVNULL)
            self.assertEqual(calls[2].kwargs['timeout'], 900)
            self.assertEqual(calls[3].args[0][-3:], ['--', 'build', './...'])
            self.assertIn(str(root / 'tooling/catalog_go.py'), calls[3].args[0])
            self.assertEqual(calls[4].kwargs['cwd'], root / 'packages/vm/tools')
            self.assertEqual(calls[5].args[0], ['go', 'build', './...'])
            self.assertTrue(all(call.kwargs['env']['GOWORK'] == 'off' for call in calls))



    def test_windows_vm_build_selects_only_compatibility_packages(self):
        environment = {'GOWORK': 'off'}
        with mock.patch.object(BUILD.subprocess, 'run') as run:
            BUILD.build_vm_tools(environment, 'Windows')
        self.assertEqual(len(run.call_args_list), 2)
        self.assertEqual(run.call_args_list[0].args[0], ['go', 'mod', 'download'])
        self.assertEqual(run.call_args_list[1].args[0], BUILD.vm_go.build_arguments('Windows'))
        self.assertNotIn('./...', run.call_args_list[1].args[0])
        self.assertIs(run.call_args_list[1].kwargs['env'], environment)


    def test_incomplete_import_graph_stops_before_offline_consumer(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.prepare_repository(root)

            def unpack(metadata, archive, expected, output):
                output.mkdir()
                (output / 'go.sum').write_text('')
                return {'source_digest': 'b' * 64}

            failure = subprocess.CalledProcessError(1, ['go', 'list'])
            with mock.patch.object(BUILD, 'ROOT', root), \
                    mock.patch.dict(os.environ, {'DOCKPIPE_GO_SDK_INPUTS': str(root / 'inputs')}), \
                    mock.patch.object(BUILD.catalog_go_dependencies.go_candidate, 'unpack_sdk', side_effect=unpack), \
                    mock.patch.object(BUILD.catalog_go_dependencies.go_candidate, 'validate_sdk_pin'), \
                    mock.patch.object(BUILD.subprocess, 'run', side_effect=[None, None, failure]) as run:
                with self.assertRaises(subprocess.CalledProcessError):
                    BUILD.main()
            self.assertEqual(len(run.call_args_list), 3)
            self.assertEqual(run.call_args_list[-1].args[0][1], 'list')


if __name__ == '__main__':
    unittest.main()
