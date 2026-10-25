import io
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'assets/scripts'))
from catalog_consumer_gate import GitHubReadOnly
from catalog_driver import admit_pipeline_environment
from catalog_transaction import PublicationStopped


class PipelineConfigurationTest(unittest.TestCase):
    def test_existing_environment_identity_is_required_without_reading_secrets(self):
        api = Mock()
        api.get.return_value = {'name': 'release-staging', 'id': 42}
        self.assertEqual(admit_pipeline_environment(api),
                         {'name': 'release-staging', 'id': 42})
        api.get.assert_called_once_with(
            'repos/Dockpipe-Industries/dockpipe-packages/environments/release-staging')

    def test_absent_or_wrong_environment_stops_before_dependent_job(self):
        for document in [{}, {'name': 'release', 'id': 42},
                         {'name': 'release-staging', 'id': True},
                         {'name': 'release-staging', 'id': 0}]:
            with self.subTest(document=document):
                api = Mock()
                api.get.return_value = document
                with self.assertRaisesRegex(PublicationStopped, 'existing|Existing'):
                    admit_pipeline_environment(api)
        api = Mock()
        api.get.side_effect = PublicationStopped('Actual qualification API read failed')
        with self.assertRaisesRegex(PublicationStopped, 'API read failed'):
            admit_pipeline_environment(api)

    def test_api_admits_only_exact_environment_metadata_not_secret_endpoints(self):
        api = GitHubReadOnly()
        with patch.object(api.opener, 'open', return_value=io.BytesIO(b'{}')) as request:
            api.get('repos/Dockpipe-Industries/dockpipe-packages/environments/release-staging')
            self.assertEqual(request.call_args.args[0].full_url,
                             'https://api.github.com/repos/Dockpipe-Industries/'
                             'dockpipe-packages/environments/release-staging')
        with patch.object(api.opener, 'open') as request:
            for suffix in ['release', 'release-staging/secrets',
                           'release-staging/variables', 'release-staging?anything=1']:
                with self.subTest(suffix=suffix):
                    with self.assertRaisesRegex(PublicationStopped, 'outside authority'):
                        api.get('repos/Dockpipe-Industries/dockpipe-packages/environments/' + suffix)
            request.assert_not_called()

    def test_prepare_checks_configuration_before_candidate_acquisition_or_output(self):
        source = (Path(__file__).resolve().parents[1] /
                  'assets/scripts/catalog_driver.py').read_text()
        body = source.split('def prepare_stage(arguments):', 1)[1].split('def ', 1)[0]
        self.assertLess(body.index('admit_pipeline_environment(api)'),
                        body.index('admit_candidate('))
        self.assertLess(body.index('admit_pipeline_environment(api)'), body.index('root.mkdir('))


if __name__ == '__main__':
    unittest.main()
