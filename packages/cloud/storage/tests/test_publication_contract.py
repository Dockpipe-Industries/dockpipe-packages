"""Exercise real Dockpipe script resolution with nonpublishing vendor-CLI fixtures.

Select an absolute released DOCKPIPE_BIN and an isolated DOCKPIPE_GLOBAL_ROOT
containing the declared public dorkpipe and onepassword resolver artifacts.
Only nonsecret fixtures and mock vendor/Terraform commands run; no provider network or source implementation fallbacks are used.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import textwrap
import unittest

FAMILY = Path(__file__).resolve().parents[1]
CATALOG = FAMILY.parents[2]
HELPER_SHA256 = '969067f0aa827fb80dfb5d6d5f819ab253d7106a1009d8275e037c788aad5b5b'


class PublicationContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cli = Path(os.environ.get('DOCKPIPE_BIN', ''))
        cls.global_root = Path(os.environ.get('DOCKPIPE_GLOBAL_ROOT', ''))
        if not cls.cli.is_absolute() or not cls.cli.is_file() or not os.access(cls.cli, os.X_OK):
            raise ValueError('Select the exact public CLI using absolute DOCKPIPE_BIN')
        if not cls.global_root.is_absolute() or not cls.global_root.is_dir():
            raise ValueError('Select an isolated explicitly populated DOCKPIPE_GLOBAL_ROOT')

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='r2-contract-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.workspace = self.root / 'workspace'
        self.workspace.mkdir()
        (self.workspace / 'dockpipe.config.json').write_text('{"schema":1}\n')
        artifacts = self.workspace / 'release/artifacts'
        artifacts.mkdir(parents=True)
        (artifacts / 'fixture.txt').write_text('nonsecret publication fixture\n')
        (self.workspace / '.env.op.template').write_text(
            'CLOUDFLARE_API_TOKEN=nonsecret-fixture-token\n'
            'CLOUDFLARE_ACCOUNT_ID=11111111111111111111111111111111\n'
            'R2_BUCKET=nonsecret-fixture\nR2_SKIP_TERRAFORM=1\n'
            'R2_ARCHIVE_NAME=fixture.tar.gz\n')
        self.tools = self.root / 'tools'
        self.tools.mkdir()
        launcher = self.tools / 'dockpipe'
        launcher.symlink_to(self.cli)
        self.receipt = self.root / 'vendor-fixture.json'
        self.write_tool('op', '''
            import os, shutil, sys
            from pathlib import Path
            arguments = sys.argv[1:]
            if os.environ.get('R2_FIXTURE_OP_FAIL') == '1':
                sys.exit(23)
            assert arguments[0] == 'inject'
            shutil.copyfile(Path(arguments[arguments.index('-i') + 1]),
                            Path(arguments[arguments.index('-o') + 1]))
        ''')
        self.write_tool('wrangler', '''
            import json, os, sys, tarfile
            from pathlib import Path
            arguments = sys.argv[1:]
            assert arguments[:3] == ['r2', 'object', 'put']
            archive = Path(arguments[arguments.index('--file') + 1])
            with tarfile.open(archive) as source:
                names = source.getnames()
                assert source.extractfile('artifacts/fixture.txt').read() == b'nonsecret publication fixture\\n'
            Path(os.environ['R2_FIXTURE_RECEIPT']).write_text(json.dumps({
                'object': arguments[3], 'members': names, 'archive': str(archive)}))
            sys.exit(int(os.environ.get('R2_FIXTURE_WRANGLER_EXIT', '0')))
        ''')
        self.write_tool('terraform', '''
            import json, os, sys
            with open(os.environ['R2_FIXTURE_TF_RECEIPT'], 'a') as stream:
                stream.write(json.dumps({'arguments': sys.argv[1:],
                                         'exit_code': int(os.environ.get('R2_FIXTURE_TF_EXIT', '0'))}) + '\\n')
            sys.exit(int(os.environ.get('R2_FIXTURE_TF_EXIT', '0')))
        ''')
        excluded = ('AWS_', 'CLOUDFLARE_', 'R2_', 'OP_', 'DOCKPIPE_', 'PIPELANG_', 'XDG_')
        self.environment = {key: value for key, value in os.environ.items()
                            if not key.startswith(excluded)}
        self.environment.update(
            DOCKPIPE_BIN=str(self.cli), DOCKPIPE_GLOBAL_ROOT=str(self.global_root),
            XDG_CACHE_HOME=str(self.root / 'cache'), XDG_CONFIG_HOME=str(self.root / 'config'),
            XDG_STATE_HOME=str(self.root / 'state'), XDG_DATA_HOME=str(self.root / 'data'),
            PATH=str(self.tools) + os.pathsep + os.defpath,
            R2_FIXTURE_RECEIPT=str(self.receipt), R2_FIXTURE_TF_RECEIPT=str(self.root / 'terraform.jsonl'))

    def write_tool(self, name, content):
        path = self.tools / name
        path.write_text('#!' + sys.executable + '\n' + textwrap.dedent(content))
        path.chmod(0o755)

    def run_cli(self, arguments):
        return subprocess.run([str(self.cli), *arguments], cwd=self.workspace,
                              env=self.environment, text=True, capture_output=True, timeout=30)

    def publication(self, name):
        workflow = FAMILY / 'resolvers/r2' / name / 'config.yml'
        return self.run_cli(['--workflow-file', str(workflow), '--workdir', str(self.workspace)])

    def test_dependency_metadata_names_actual_script_owners(self):
        package = FAMILY / 'resolvers/r2/secretstore-r2-publish-test/package.yml'
        text = package.read_text()
        self.assertIn('depends: [dorkpipe, onepassword, dockpipe.cloudflare.r2infra]', text)
        self.assertIn('requires_resolvers: [onepassword, dorkpipe]', text)
        for name in ('secretstore-r2-publish-test', 'dockpipe.cloudflare.r2upload'):
            workflow = FAMILY / 'resolvers/r2' / name / 'config.yml'
            self.assertIn('scripts/dorkpipe/r2-publish.sh', workflow.read_text())
            self.assertNotIn('scripts/dockpipe/r2-publish.sh', workflow.read_text())

    def test_umbrella_resolver_lists_have_actual_profiles(self):
        for manifest in (CATALOG / 'packages').rglob('package.yml'):
            text = manifest.read_text()
            match = re.search(r'^includes_resolvers:\s*\n((?:[ \t]+- [^\n]+\n)+)', text, re.MULTILINE)
            if not match:
                continue
            for name in re.findall(r'^\s+-\s+(\S+)', match[1], re.MULTILINE):
                resolver = manifest.parent / 'resolvers' / name
                metadata = resolver / 'package.yml'
                declared = metadata.is_file() and re.search(r'^kind: resolver\s*$', metadata.read_text(), re.MULTILINE)
                with self.subTest(package=str(manifest), resolver=name):
                    self.assertTrue((resolver / 'profile').is_file() or declared)

    def test_actual_published_helper_is_selected_without_source_fallback(self):
        result = self.run_cli(['scope', 'resolver', 'dorkpipe', 'assets-dir', '--workdir', str(self.workspace)])
        self.assertEqual(result.returncode, 0, result.stderr)
        assets = Path(result.stdout.strip())
        self.assertTrue(assets.is_relative_to(self.workspace))
        helper = assets / 'scripts/r2-publish.sh'
        self.assertEqual(hashlib.sha256(helper.read_bytes()).hexdigest(), HELPER_SHA256)
        self.assertFalse((self.workspace / 'packages').exists())

    def test_secretstore_outputs_reach_real_helper_and_mock_upload(self):
        result = self.publication('secretstore-r2-publish-test')
        self.assertEqual(result.returncode, 0, result.stderr)
        receipt = json.loads(self.receipt.read_text())
        self.assertEqual(receipt['object'], 'nonsecret-fixture/fixture.tar.gz')
        self.assertIn('artifacts/fixture.txt', receipt['members'])
        self.assertFalse(Path(receipt['archive']).exists(), 'Helper must remove its temporary archive')

    def test_installed_default_mode_resolves_infra_and_public_terraform_workflow(self):
        template = self.workspace / '.env.op.template'
        template.write_text('\n'.join(line for line in template.read_text().splitlines()
                                       if not line.startswith('R2_SKIP_TERRAFORM=')) + '\n')
        result = self.run_cli(['--workflow', 'secretstore-r2-publish-test',
                               '--workdir', str(self.workspace)])
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = [json.loads(line) for line in (self.root / 'terraform.jsonl').read_text().splitlines()]
        self.assertTrue(any('init' in command['arguments'] for command in commands))
        self.assertTrue(any('apply' in command['arguments'] for command in commands))
        self.assertEqual(json.loads(self.receipt.read_text())['object'], 'nonsecret-fixture/fixture.tar.gz')
        self.assertFalse((self.workspace / 'packages').exists())

    def test_default_terraform_failure_prevents_upload(self):
        template = self.workspace / '.env.op.template'
        template.write_text('\n'.join(line for line in template.read_text().splitlines()
                                       if not line.startswith('R2_SKIP_TERRAFORM=')) + '\n')
        self.environment['R2_FIXTURE_TF_EXIT'] = '31'
        result = self.run_cli(['--workflow', 'secretstore-r2-publish-test',
                               '--workdir', str(self.workspace)])
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((self.root / 'terraform.jsonl').exists())
        self.assertFalse(self.receipt.exists())

    def test_upload_workflow_retains_explicit_skip_default(self):
        workflow = FAMILY / 'resolvers/r2/dockpipe.cloudflare.r2upload/config.yml'
        arguments = ['--workflow-file', str(workflow), '--workdir', str(self.workspace)]
        for name, value in (
            ('R2_BUCKET', 'nonsecret-fixture'), ('R2_ARCHIVE_NAME', 'fixture.tar.gz'),
            ('CLOUDFLARE_API_TOKEN', 'nonsecret-fixture-token'),
            ('CLOUDFLARE_ACCOUNT_ID', '11111111111111111111111111111111'),
        ):
            arguments.extend(['--var', name + '=' + value])
        result = self.run_cli(arguments)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.receipt.exists())
        self.assertFalse((self.root / 'terraform.jsonl').exists())

    def test_vendor_failure_stops_publication_and_propagates(self):
        self.environment['R2_FIXTURE_OP_FAIL'] = '1'
        result = self.publication('secretstore-r2-publish-test')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.receipt.exists())

    def test_upload_failure_is_not_reported_as_success(self):
        self.environment['R2_FIXTURE_WRANGLER_EXIT'] = '29'
        result = self.publication('secretstore-r2-publish-test')
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(self.receipt.exists())
        self.assertNotIn('uploaded r2://', result.stdout)


if __name__ == '__main__':
    unittest.main()
