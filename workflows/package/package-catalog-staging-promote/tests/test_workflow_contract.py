from pathlib import Path
import unittest
import yaml

ROOT = Path(__file__).resolve().parents[4]


class WorkflowContractTest(unittest.TestCase):
    def setUp(self):
        self.document = yaml.load((ROOT / '.github/workflows/ci.yml').read_text(), Loader=yaml.BaseLoader)
        self.jobs = self.document['jobs']

    def test_registered_workflow_keeps_all_normal_source_events(self):
        events = self.document['on']
        self.assertEqual(events['push']['branches'], ['master', 'staging', 'dev'])
        self.assertEqual(events['pull_request']['branches'], ['master', 'staging', 'dev'])
        self.assertEqual(events['workflow_dispatch']['inputs']['catalog_promotion_execute']['default'], 'false')
        self.assertEqual(self.document['permissions'], {'contents': 'read'})

    def test_existing_source_jobs_retain_actual_bootstrap_and_workflow_checks(self):
        for name in ['test', 'test-windows']:
            text = '\n'.join(step.get('run', '') for step in self.jobs[name]['steps'])
            self.assertIn('tooling/bootstrap_dockpipe.py', text)
            self.assertIn('tooling/bootstrap_runtime_core.py', text)
            self.assertIn('tooling/fetch_sdk_release.py', text)
        linux = '\n'.join(step.get('run', '') for step in self.jobs['test']['steps'])
        windows = '\n'.join(step.get('run', '') for step in self.jobs['test-windows']['steps'])
        self.assertIn('workflows/codeql.build/config.yml', linux)
        self.assertIn('workflows/catalog.test/config.yml', linux)
        self.assertIn('workflows/ci.windows/config.yml', windows)
        self.assertNotIn('if', self.jobs['test'])
        self.assertNotIn('if', self.jobs['test-windows'])

    def test_only_explicit_staging_dispatch_can_reach_provider_jobs(self):
        prepare = self.jobs['catalog-prepare']
        self.assertEqual(prepare['needs'], ['test', 'test-windows', 'catalog-promotion-contracts'])
        self.assertIn("github.event_name == 'workflow_dispatch'", prepare['if'])
        self.assertIn("github.ref == 'refs/heads/staging'", prepare['if'])
        self.assertIn('inputs.catalog_promotion_execute', prepare['if'])
        self.assertEqual(self.jobs['catalog-promote']['needs'],
                         ['catalog-prepare', 'catalog-upload', 'catalog-consumer'])

    def test_all_three_real_native_consumers_required_without_weakening(self):
        consumer = self.jobs['catalog-consumer']
        self.assertEqual(consumer['name'], 'consumer (${{ matrix.platform }})')
        self.assertEqual(consumer['strategy']['fail-fast'], 'false')
        self.assertEqual({item['platform'] for item in consumer['strategy']['matrix']['include']},
                         {'linux-amd64', 'windows-amd64', 'darwin-arm64'})
        self.assertEqual(consumer['needs'], ['catalog-prepare', 'catalog-upload'])
        for name, job in self.jobs.items():
            if name.startswith('catalog-'):
                self.assertNotIn('continue-on-error', job)
                if name in {'catalog-upload', 'catalog-promote'}:
                    self.assertEqual(job['environment'], 'release-staging')
                else:
                    self.assertNotIn('environment', job)
                for step in job['steps']:
                    self.assertNotIn('continue-on-error', step)
        for name in ['catalog-prepare', 'catalog-consumer', 'catalog-promotion-contracts']:
            self.assertNotIn('secrets.', str(self.jobs[name]))

    def test_current_run_artifacts_and_literal_secret_refs_only(self):
        for name in ['catalog-upload', 'catalog-consumer', 'catalog-promote']:
            for step in self.jobs[name]['steps']:
                if step.get('uses') == 'actions/download-artifact@v4':
                    self.assertIn('github.run_id', step['with']['name'])
                    self.assertIn('github.run_attempt', step['with']['name'])
                    self.assertNotIn('run-id', step['with'])
        for name in ['catalog-upload', 'catalog-promote']:
            environments = [step['env'] for step in self.jobs[name]['steps'] if 'env' in step]
            self.assertEqual(len(environments), 1)
            self.assertEqual(environments[0]['AWS_ACCESS_KEY_ID'],
                             '${{ secrets.AWS_ACCESS_KEY_ID }}')
            self.assertEqual(environments[0]['AWS_SECRET_ACCESS_KEY'],
                             '${{ secrets.AWS_SECRET_ACCESS_KEY }}')
            self.assertEqual(environments[0]['R2_ENDPOINT_URL'], '${{ vars.R2_ENDPOINT_URL }}')
            self.assertEqual(environments[0]['DOCKPIPE_RELEASE_BUCKET'],
                             '${{ vars.DOCKPIPE_RELEASE_BUCKET }}')

    def test_api_token_is_step_scoped_to_three_admission_stages(self):
        for name in ['catalog-prepare', 'catalog-upload', 'catalog-promote']:
            job = self.jobs[name]
            self.assertEqual(job['permissions'], {'contents': 'read', 'actions': 'read'})
            token_steps = [step for step in job['steps'] if 'GH_TOKEN' in step.get('env', {})]
            self.assertEqual(len(token_steps), 1)
            self.assertEqual(token_steps[0]['env']['GH_TOKEN'], '${{ github.token }}')
        consumer = self.jobs['catalog-consumer']
        self.assertNotIn('GH_TOKEN', str(consumer))
        self.assertNotIn('env', consumer)

    def test_native_consumer_has_only_sparse_helper_checkout_without_credentials(self):
        consumer = self.jobs['catalog-consumer']
        checkouts = [step for step in consumer['steps'] if step.get('uses') == 'actions/checkout@v4']
        self.assertEqual(len(checkouts), 1)
        self.assertEqual(checkouts[0]['with'], {
            'persist-credentials': 'false',
            'sparse-checkout-cone-mode': 'false',
            'sparse-checkout': '/workflows/package/package-catalog-staging-promote/assets/scripts/*.py\n',
        })
        self.assertNotIn('secrets.', str(consumer))

    def test_owned_yaml_entrypoint_is_native_python_host_only(self):
        path = ROOT / 'workflows/package/package-catalog-staging-promote/config.yml'
        config = yaml.safe_load(path.read_text())
        self.assertFalse(config['docker_preflight'])
        self.assertEqual(config['steps'], [{'id': 'promote-optional-catalog', 'kind': 'host',
                                           'run': 'assets/scripts/workflow_entry.py'}])


if __name__ == '__main__':
    unittest.main()
