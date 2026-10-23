"""Stop the actual shell pipeline on required-command failure, including guarded callers."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest

PIPELINE = Path(__file__).resolve().parents[1] / 'assets/scripts/terraform-pipeline.sh'


class PipelineFailureTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='terraform-contract-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.module = self.root / 'module'
        self.module.mkdir()
        self.log = self.root / 'commands.jsonl'
        tool = self.root / 'terraform'
        tool.write_text('#!' + sys.executable + '\n' + textwrap.dedent('''
            import json, os, sys
            arguments = sys.argv[1:]
            with open(os.environ['TF_FIXTURE_LOG'], 'a') as stream:
                stream.write(json.dumps(arguments) + '\\n')
            command = ' '.join(arguments[:2]) if arguments[0] == 'workspace' else arguments[0]
            failures = os.environ['TF_FIXTURE_FAIL'].split(',')
            sys.exit(23 if command in failures else 0)
        '''))
        tool.chmod(0o755)
        self.environment = {
            'PATH': str(self.root) + os.pathsep + os.defpath,
            'TF_FIXTURE_LOG': str(self.log), 'TF_FIXTURE_FAIL': '',
            'DOCKPIPE_TF_BACKEND': 'local', 'DOCKPIPE_TF_COMMANDS': 'init,plan,apply',
        }

    def run_pipeline(self):
        # A caller's conditional also suppresses errexit. The function must
        # deliberately propagate failure instead of depending on shell options.
        return subprocess.run([
            'bash', '-c', 'source "$1"; if dockpipe_tf_run_pipeline "$2" "$3"; then exit 0; else exit 7; fi',
            'pipeline-fixture', str(PIPELINE), str(self.module), str(self.root / 'backend.hcl'),
        ], env=self.environment, capture_output=True, text=True, timeout=10)

    def commands(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def test_first_failed_required_command_stops_later_operations(self):
        for failed in ('init', 'plan', 'apply', 'validate', 'fmt'):
            with self.subTest(command=failed):
                self.log.unlink(missing_ok=True)
                self.environment.update(TF_FIXTURE_FAIL=failed, DOCKPIPE_TF_COMMANDS='init,' + failed + ',apply')
                result = self.run_pipeline()
                self.assertNotEqual(result.returncode, 0)
                calls = self.commands()
                self.assertEqual(calls[-1][0], failed)
                self.assertEqual(sum(command[0] == failed for command in calls), 1)

    def test_failed_import_file_stops_remaining_imports_and_apply(self):
        imports = self.root / 'imports.txt'
        imports.write_text('resource.first first-id\nresource.second second-id\n')
        self.environment.update(DOCKPIPE_TF_COMMANDS='init,import,apply', TF_FIXTURE_FAIL='import',
                                DOCKPIPE_TF_IMPORT_FILE=str(imports))
        result = self.run_pipeline()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual([command[0] for command in self.commands()], ['init', 'import'])

    def test_workspace_select_can_fall_back_but_failed_creation_stops_apply(self):
        self.environment.update(DOCKPIPE_TF_WORKSPACE='fixture', TF_FIXTURE_FAIL='workspace select,workspace new')
        result = self.run_pipeline()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual([command[:2] for command in self.commands()],
                         [['init', '-input=false'], ['workspace', 'select'], ['workspace', 'new']])
        self.log.unlink()
        self.environment['TF_FIXTURE_FAIL'] = 'workspace select'
        result = self.run_pipeline()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.commands()[-1][0], 'apply')

    def test_missing_module_never_runs_terraform_in_the_wrong_directory(self):
        self.module.rmdir()
        result = self.run_pipeline()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.commands(), [])

    def test_successful_pipeline_keeps_command_order(self):
        result = self.run_pipeline()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([command[0] for command in self.commands()], ['init', 'plan', 'apply'])


if __name__ == '__main__':
    unittest.main()
