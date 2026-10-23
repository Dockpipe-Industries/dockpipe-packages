"""Admission tests only; mocked observations are not hosted native build proof."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

TOOLING = Path(__file__).resolve().parents[1] / 'tooling'
sys.path.insert(0, str(TOOLING))
specification = importlib.util.spec_from_file_location('guest_build', TOOLING / 'build_candidate.py')
emitter = importlib.util.module_from_spec(specification)
specification.loader.exec_module(emitter)
SOURCE = '1' * 40


class CandidateBuildTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def environment(self):
        return {'GITHUB_ACTIONS': 'true', 'RUNNER_OS': 'Windows', 'RUNNER_ARCH': 'X64',
                'GITHUB_REPOSITORY': emitter.candidate.REPOSITORY, 'GITHUB_RUN_ID': '123',
                'GITHUB_RUN_ATTEMPT': '1', 'GITHUB_WORKFLOW_SHA': '2' * 40}

    def test_hosted_workflow_and_artifact_source_are_independent(self):
        result = emitter.hosted_identity(self.environment())
        self.assertEqual(result['run_id'], 123)
        self.assertNotEqual(result['workflow_sha'], SOURCE)

    def test_local_wrong_repository_and_incomplete_workflow_are_rejected(self):
        for key, value in [('GITHUB_ACTIONS', 'false'), ('RUNNER_OS', 'Linux'),
                           ('RUNNER_ARCH', 'ARM64'), ('GITHUB_REPOSITORY', 'other/repository'),
                           ('GITHUB_RUN_ID', '0'), ('GITHUB_RUN_ATTEMPT', '101'),
                           ('GITHUB_WORKFLOW_SHA', 'development')]:
            with self.subTest(key=key):
                environment = self.environment()
                environment[key] = value
                with self.assertRaises(ValueError):
                    emitter.hosted_identity(environment)

    def ctest(self, first='', second='', suite=''):
        path = self.root / 'tests.xml'
        path.write_text('<testsuites><testsuite ' + suite + '>'
                        '<testcase name="guest-tools-status" ' + first + '/>'
                        '<testcase name="guest-tools-clipboard" ' + second + '/>'
                        '</testsuite></testsuites>')
        return path

    def test_both_named_native_tests_must_actually_run(self):
        result = emitter.ctest_outcomes(self.ctest('status="run"', 'status="run"'))
        self.assertEqual(result, {'guest-tools-status': 'passed', 'guest-tools-clipboard': 'passed'})
        for first, second, suite in [('status="notrun"', '', ''), ('', '', 'failures="1"'),
                                     ('', '', 'skipped="1"'), ('', '', 'disabled="1"')]:
            with self.subTest(first=first, suite=suite):
                with self.assertRaises(ValueError):
                    emitter.ctest_outcomes(self.ctest(first, second, suite))

    def test_skipped_failure_missing_and_duplicate_tests_are_rejected(self):
        path = self.root / 'tests.xml'
        for body in ['<testcase name="guest-tools-status"><skipped/></testcase>',
                     '<testcase name="guest-tools-status"><failure/></testcase>',
                     '',
                     '<testcase name="guest-tools-clipboard"/>']:
            with self.subTest(body=body):
                path.write_text('<testsuite>' + body
                                + '<testcase name="guest-tools-clipboard"/></testsuite>')
                with self.assertRaises(ValueError):
                    emitter.ctest_outcomes(path)

    def snapshot(self, overrides=None):
        component = self.root / emitter.COMPONENT
        component.mkdir(parents=True, exist_ok=True)
        (component / 'source.cpp').write_bytes(b'committed\n')
        (self.root / 'LICENSE').write_bytes(b'license\n')
        observations = {
            'rev-parse --show-toplevel': str(self.root).encode() + b'\n',
            'rev-parse HEAD': SOURCE.encode() + b'\n',
            'remote get-url origin': ('https://github.com/' + emitter.candidate.REPOSITORY + '.git\n').encode(),
            'status --porcelain=v1 --untracked-files=all': b'',
            'ls-files -z -- packages/vm/guest-tools': b'packages/vm/guest-tools/source.cpp\0',
            'show ' + SOURCE + ':packages/vm/guest-tools/source.cpp': b'committed\n',
            'show ' + SOURCE + ':LICENSE': b'license\n',
        }
        observations.update(overrides or {})
        commands = []

        def capture(arguments, maximum=262144):
            self.assertEqual(arguments[1:4], ['--no-optional-locks', '-C', self.root])
            command = ' '.join(str(value) for value in arguments[4:])
            commands.append(command)
            return observations[command]

        with mock.patch.object(emitter, 'capture', side_effect=capture):
            result = emitter.source_snapshot(self.root, Path('/explicit/git'), SOURCE)
        self.assertTrue(all(command.split()[0] in {'rev-parse', 'remote', 'status', 'ls-files', 'show'}
                            for command in commands))
        return result

    def test_source_inventory_is_exact_committed_material_without_git_mutation(self):
        self.assertEqual(self.snapshot(), [{'path': 'source.cpp',
                                          'sha256': emitter.candidate.digest(b'committed\n')}])

    def test_dirty_wrong_commit_remote_and_checkout_conversion_are_rejected(self):
        overrides = [
            {'status --porcelain=v1 --untracked-files=all': b' M source.cpp\n'},
            {'rev-parse HEAD': b'2' * 40},
            {'remote get-url origin': b'https://example.invalid/repository'},
            {'show ' + SOURCE + ':packages/vm/guest-tools/source.cpp': b'committed\r\n'},
        ]
        for observation in overrides:
            with self.subTest(observation=observation):
                with self.assertRaises(ValueError):
                    self.snapshot(observation)

    def test_ignored_material_source_is_rejected(self):
        self.snapshot()
        (self.root / emitter.COMPONENT / 'ignored.bin').write_bytes(b'not committed')
        with self.assertRaisesRegex(ValueError, 'ignored/generated'):
            self.snapshot()

    def test_non_native_build_refused_before_any_output_or_subprocess(self):
        with mock.patch.object(emitter.platform, 'system', return_value='Linux'), \
                mock.patch.object(emitter.subprocess, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'Native Windows'):
                emitter.build(self.root, SOURCE, Path('/git'), Path('/cli'), '0' * 64,
                              self.root / 'qt', self.root / 'output')
            run.assert_not_called()
            self.assertFalse((self.root / 'output').exists())


if __name__ == '__main__':
    unittest.main()
