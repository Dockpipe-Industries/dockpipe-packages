"""Keep Windows compatibility distinct from the Linux VM qualification lane."""
import importlib.util
from pathlib import Path
import unittest
from unittest import mock

import vm_go

SCRIPT = Path(__file__).resolve().parents[1] / 'workflows/ci.windows/assets/scripts/test.py'
SPEC = importlib.util.spec_from_file_location('catalog_windows_acceptance', SCRIPT)
WINDOWS_RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(WINDOWS_RUNNER)


class VMHostMatrixTests(unittest.TestCase):
    def test_linux_build_and_tests_keep_the_complete_module(self):
        self.assertEqual(vm_go.build_arguments('Linux'), ['go', 'build', './...'])
        self.assertEqual(vm_go.test_arguments('Linux'), [['go', 'test', './...']])

    def test_windows_build_names_the_supported_compatibility_packages(self):
        self.assertEqual(vm_go.build_arguments('Windows'), [
            'go', 'build', './internal/protocol', './internal/controller',
            './internal/manifest', './internal/keymaterial', './internal/recovery',
            './internal/guest', './cmd/dockpipe-guest-agent',
        ])

    def test_windows_tests_preserve_the_linux_filesystem_contract_boundary(self):
        commands = vm_go.test_arguments('Windows')
        self.assertEqual(commands[0], [
            'go', 'test', './internal/protocol', './internal/controller',
            './internal/manifest', './internal/guest', './cmd/dockpipe-guest-agent',
        ])
        self.assertEqual(commands[1][2:4], ['./internal/recovery', '-run'])
        self.assertIn('TestRecoveryRejectsSubstitutionAndDurableWriteFailure', commands[1][4])
        self.assertEqual(commands[2][2:4], ['./internal/keymaterial', '-run'])
        self.assertNotIn('TestFileStoreUsesOwnerOnlyTicketFiles', commands[1][4])
        self.assertNotIn('TestGeneratePerInstanceOwnerOnlyKey', commands[2][4])

    def test_unknown_host_fails_instead_of_claiming_linux_qualification(self):
        for operation in (vm_go.build_arguments, vm_go.test_arguments):
            with self.assertRaises(ValueError):
                operation('Other')

    def test_windows_workflow_requires_native_windows(self):
        with mock.patch.object(WINDOWS_RUNNER.platform, 'system', return_value='Linux'), \
                mock.patch.object(WINDOWS_RUNNER.subprocess, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'native Windows'):
                WINDOWS_RUNNER.main()
        run.assert_not_called()

    def test_windows_workflow_builds_catalog_tests_consumers_and_tests_vm_slice(self):
        with mock.patch.object(WINDOWS_RUNNER.platform, 'system', return_value='Windows'), \
                mock.patch.object(WINDOWS_RUNNER.subprocess, 'run') as run:
            WINDOWS_RUNNER.main()
        calls = run.call_args_list
        self.assertEqual(len(calls), 5)
        self.assertTrue(calls[0].args[0][1].endswith('workflows/codeql.build/assets/scripts/build.py'))
        self.assertEqual(calls[1].args[0][-3:], [
            'test', './packages/secrets/tools/...', './packages/remote/tools/...'])
        self.assertEqual([call.args[0] for call in calls[2:]], vm_go.test_arguments('Windows'))
        self.assertTrue(all(call.kwargs['env']['GOWORK'] == 'off' for call in calls))


if __name__ == '__main__':
    unittest.main()
