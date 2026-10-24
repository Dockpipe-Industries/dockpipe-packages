import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'assets/scripts'))
from catalog_consumer import SCRIPT_DIRECTORY, assert_source_hidden, verify_consumer
from catalog_consumer_gate import GitHubReadOnly, NoRedirect
from catalog_plan import json_bytes
from catalog_transaction import PublicationStopped


class APIAuthenticationTest(unittest.TestCase):
    def test_optional_token_only_reaches_fixed_api_endpoint(self):
        with patch.dict('os.environ', {'GH_TOKEN': 'ephemeral-fixture-token'}, clear=True):
            api = GitHubReadOnly()
        self.assertTrue(any(isinstance(handler, NoRedirect) for handler in api.opener.handlers))
        with patch.object(api.opener, 'open', return_value=io.BytesIO(b'{}')) as request:
            api.get('repos/Dockpipe-Industries/dockpipe-packages/actions/runs/123')
            admitted = request.call_args.args[0]
            self.assertEqual(admitted.full_url,
                             'https://api.github.com/repos/Dockpipe-Industries/dockpipe-packages/actions/runs/123')
            self.assertEqual(admitted.get_header('Authorization'), 'Bearer ephemeral-fixture-token')
            self.assertEqual(request.call_args.kwargs['timeout'], 30)
        with patch.object(api.opener, 'open') as request:
            for endpoint in ['https://foreign.example/actions/runs/123',
                             'repos/another/repository/actions/runs/123',
                             'repos/Dockpipe-Industries/dockpipe-packages/actions/runs/123?next=foreign']:
                with self.assertRaisesRegex(PublicationStopped, 'outside authority'):
                    api.get(endpoint)
            request.assert_not_called()

    def test_anonymous_mode_has_no_authorization_header(self):
        with patch.dict('os.environ', {}, clear=True):
            api = GitHubReadOnly()
        with patch.object(api.opener, 'open', return_value=io.BytesIO(b'{}')) as request:
            api.get('repos/Dockpipe-Industries/dockpipe-packages/releases/123')
        self.assertIsNone(request.call_args.args[0].get_header('Authorization'))

    def test_redirect_fails_without_forwarding_token(self):
        request = urllib.request.Request('https://api.github.com/admitted',
                                         headers={'Authorization': 'Bearer sentinel'})
        with self.assertRaisesRegex(PublicationStopped, 'redirect') as failure:
            NoRedirect().redirect_request(request, None, 302, 'redirect', {},
                                          'https://foreign.example/')
        self.assertNotIn('sentinel', str(failure.exception))

    def test_invalid_token_is_rejected_without_value_in_error(self):
        with patch.dict('os.environ', {'GH_TOKEN': 'sentinel\r\nInjected: value'}, clear=True):
            with self.assertRaisesRegex(PublicationStopped, 'token reference') as failure:
                GitHubReadOnly()
        self.assertNotIn('sentinel', str(failure.exception))


    def test_github_token_never_reaches_provider_child(self):
        from catalog_s3 import S3Provider

        class FinishedProcess:
            returncode = 0

            def poll(self):
                return 0

        with tempfile.TemporaryDirectory() as directory:
            provider = S3Provider.__new__(S3Provider)
            provider.temporary_root = Path(directory)
            provider.executable = Path('/admitted/aws')
            provider.endpoint = 'https://' + 'a' * 32 + '.r2.cloudflarestorage.com'
            with patch.dict('os.environ', {'GH_TOKEN': 'github-sentinel',
                                          'AWS_ACCESS_KEY_ID': 'fixture-access',
                                          'AWS_SECRET_ACCESS_KEY': 'fixture-secret'}, clear=True):
                with patch('catalog_s3.subprocess.Popen', return_value=FinishedProcess()) as execute:
                    provider.command(['s3api', 'put-object', '--generate-cli-skeleton', 'input'])
            environment = execute.call_args.kwargs['env']
            self.assertNotIn('GH_TOKEN', environment)
            self.assertNotIn('github-sentinel', str(execute.call_args))
            self.assertEqual(environment['AWS_MAX_ATTEMPTS'], '1')



class SourceHiddenTest(unittest.TestCase):
    def helper_checkout(self, root):
        scripts = root / SCRIPT_DIRECTORY
        scripts.mkdir(parents=True)
        (scripts / 'catalog_driver.py').write_text('# fixture helper\n')
        (root / '.git').mkdir()
        return root

    def test_only_helper_checkout_is_admitted(self):
        with tempfile.TemporaryDirectory() as directory:
            root = self.helper_checkout(Path(directory))
            receipt = assert_source_hidden(root)
            self.assertTrue(receipt['only_helper_scripts'])
            self.assertEqual(len(receipt['files']), 1)
            self.assertEqual(receipt['files'][0]['path'],
                             (SCRIPT_DIRECTORY / 'catalog_driver.py').as_posix())

    def test_actual_source_present_fails_before_any_cli_execution(self):
        for forbidden in ['packages', 'src', 'tooling', 'source']:
            with self.subTest(forbidden=forbidden), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                checkout = self.helper_checkout(root / 'checkout')
                (checkout / forbidden).mkdir()
                with patch('catalog_consumer.subprocess.Popen') as execution:
                    with self.assertRaisesRegex(PublicationStopped, 'source tree is present'):
                        verify_consumer(Path('/not/executed'), '0' * 64, 'version', root / 'consumer',
                                        {}, 'linux-amd64', {}, [], {}, checkout)
                    execution.assert_not_called()

    def test_root_config_or_helper_alias_is_not_source_hidden(self):
        with tempfile.TemporaryDirectory() as directory:
            root = self.helper_checkout(Path(directory))
            (root / 'dockpipe.config.json').write_text('{}')
            with self.assertRaisesRegex(PublicationStopped, 'non-helper'):
                assert_source_hidden(root)

    def test_consumer_receipt_follows_checked_absence_and_secrets_do_not_reach_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            checkout = self.helper_checkout(root / 'checkout')
            executable = root / 'dockpipe'
            executable.write_bytes(b'admitted-test-bytes')
            digest = hashlib.sha256(executable.read_bytes()).hexdigest()
            manifest = {'schema': 1, 'platform': 'linux-amd64',
                        'packages': {'resolvers': [], 'workflows': []}}
            plan = {'objects': [{'role': 'release-manifest', 'key': 'packages/catalog/fixture.json',
                                 'sha256': '0' * 64, 'size_bytes': 1}]}
            calls = []

            class FinishedProcess:
                returncode = 0

                def poll(self):
                    return 0

            def launch(arguments, **keywords):
                calls.append(keywords['env'])
                payload = b'dockpipe fixture-version\n' if arguments[1] == '--version' else json_bytes({
                    'platform': 'linux-amd64', 'packages': []})
                keywords['stdout'].write(payload)
                return FinishedProcess()

            with patch.dict('os.environ', {'GH_TOKEN': 'github-sentinel',
                                          'AWS_SECRET_ACCESS_KEY': 'aws-sentinel'}, clear=False):
                with patch('catalog_consumer.subprocess.Popen', side_effect=launch):
                    receipt = verify_consumer(executable, digest, 'fixture-version', root / 'consumer',
                                              plan, 'linux-amd64', manifest, [], {}, checkout)
            self.assertTrue(receipt['source_hidden'])
            self.assertTrue(receipt['source_checkout_admission']['only_helper_scripts'])
            self.assertEqual(len(calls), 2)
            for environment in calls:
                self.assertNotIn('GH_TOKEN', environment)
                self.assertNotIn('AWS_SECRET_ACCESS_KEY', environment)


if __name__ == '__main__':
    unittest.main()
