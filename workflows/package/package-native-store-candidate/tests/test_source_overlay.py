"""Source-only repair never inherits native artifact claims or weakens publication."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from test_native_store import SCRIPTS
from test_publish_candidate import FakeGitHub
import native_store
import publication_plan
import source_overlay


class SourceOverlayTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.commit = 'a' * 40
        self.template = Path(__file__).resolve().parents[4] / 'release/source-overlay.lock.json'
        self.lock = native_store.read_json(self.template)
        self.lock['source_commit'] = self.commit
        self.lock['inputs']['source'] = dict(self.lock['inputs']['cli'],
            url='https://github.com/' + native_store.REPOSITORY + '/archive/' + self.commit + '.tar.gz',
            root='dockpipe-packages-' + self.commit)
        self.lock_path = self.root / 'lock.json'
        self.save_lock()
        self.environment = {'GITHUB_REPOSITORY': native_store.REPOSITORY, 'GITHUB_SHA': self.commit,
                            'GITHUB_REF': 'refs/heads/dev', 'GITHUB_EVENT_NAME': 'push'}

    def save_lock(self):
        self.lock_path.write_text(json.dumps(self.lock))

    def candidate(self):
        flat = self.root / 'flat'
        flat.mkdir()
        entries = []
        for workflow in self.lock['workflows']:
            filename = 'dockpipe-workflow-' + workflow['name'] + '-' + workflow['version'] + '.tar.gz'
            payload = workflow['name'].encode()
            (flat / filename).write_bytes(payload)
            entries.append({'name': workflow['name'], 'version': workflow['version'], 'tarball': filename,
                            'sha256': hashlib.sha256(payload).hexdigest(), 'size_bytes': len(payload)})
        (flat / 'packages-store-manifest.json').write_text(json.dumps(
            {'schema': 1, 'platform': 'linux-amd64', 'packages': {'workflows': entries}}))
        archive = self.root / 'dockpipe-catalog-linux-amd64.tar.gz'
        native_store.archive_flat(flat, archive)
        proof = self.root / 'consumer-proof.json'
        proof.write_text(json.dumps({'strict_import_recompose': True, 'flatpak_dependency_preflight': True, 'default_execution_tests': True}))
        descriptor = {'schema': 1, 'repository': native_store.REPOSITORY, 'source_commit': self.commit,
            'source_state': 'committed-snapshot', 'source_archive_sha256': self.lock['inputs']['source']['sha256'],
            'tag': 'catalog-candidate-' + self.commit, 'owned': source_overlay.OWNED,
            'platform': 'linux-amd64', 'publication_mode': 'source-only', 'native_build': False, 'source_only': True, 'published': False,
            'checks': source_overlay.CHECKS, 'inputs_lock_sha256': native_store.sha256(self.lock_path),
            'asset': archive.name, 'sha256': native_store.sha256(archive), 'size_bytes': archive.stat().st_size,
            'manifest_sha256': native_store.sha256(flat / 'packages-store-manifest.json'),
            'consumer_proof_sha256': native_store.sha256(proof)}
        candidate = self.root / 'candidate.json'
        candidate.write_text(json.dumps(descriptor))
        return candidate

    def publish(self, candidate, client, suffix='publication', attestor=None):
        return source_overlay.publish(candidate, self.lock_path, self.root / suffix, client,
                                      self.environment, attestor or (lambda path, source: None))

    def test_historical_reproduction_is_explicit_manual_only_without_publication(self):
        import yaml
        workflow = self.template.parents[1] / '.github/workflows/source-store-candidate.yml'
        document = yaml.safe_load(workflow.read_text())
        build = document['jobs']['build']
        install = next(step for step in build['steps'] if step.get('name', '').startswith('Install only'))
        self.assertIn('--only-binary=:all:', install['run'])
        events = document.get('on', document.get(True))
        self.assertEqual(set(events), {'workflow_dispatch'})
        self.assertNotIn('publish', document['jobs'])

    def test_complete_template_has_only_real_public_runtime_dependencies(self):
        source_overlay.load_lock(self.template, template=True)
        source_overlay.load_lock(self.lock_path)
        self.assertEqual(set(self.lock['inputs']), source_overlay.ROLES)
        self.assertNotIn('qt', self.lock['inputs'])
        self.assertNotIn('guest_tools', self.lock['inputs'])
        self.assertEqual(self.lock['inputs']['verifier_source']['size_bytes'], 8344375)

    def test_extra_ownership_wrong_source_mutable_store_or_wrong_version_fails(self):
        original = copy.deepcopy(self.lock)
        for mutation in ('ownership', 'source', 'url', 'version'):
            self.lock = copy.deepcopy(original)
            if mutation == 'ownership':
                self.lock['owned'].append(['resolver', 'r2'])
            elif mutation == 'source':
                self.lock['inputs']['source']['url'] = self.lock['inputs']['cli']['url']
            elif mutation == 'url':
                self.lock['inputs']['dorkpipe']['url'] = 'https://packages.staging.dockpipe.com/latest'
            else:
                self.lock['workflows'][0]['version'] = '0.6.2'
            self.save_lock()
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                source_overlay.load_lock(self.lock_path)

    def test_source_preparation_binds_one_accepted_archive_without_self_reference(self):
        def download(url, destination):
            self.assertIn(self.commit, url)
            destination.write_bytes(b'public committed source fixture')
        result = source_overlay.prepare(self.template, self.root / 'prepared', self.environment, download)
        effective = source_overlay.load_lock(result)
        self.assertEqual(effective['source_commit'], self.commit)
        self.assertEqual(effective['inputs']['source']['sha256'], hashlib.sha256(b'public committed source fixture').hexdigest())
        self.assertIsNone(native_store.read_json(self.template)['source_commit'])

    def test_source_candidate_is_refused_by_native_publication_mode(self):
        candidate = self.candidate()
        with self.assertRaises(ValueError):
            publication_plan.plan([candidate], qualification='native')
        plan = publication_plan.plan([candidate], qualification='source-only')
        self.assertEqual(plan['owned'], source_overlay.OWNED)
        self.assertEqual(set(plan['artifacts']), {'linux-amd64'})

    def test_immutable_publish_binds_attestations_and_remains_explicitly_source_only(self):
        candidate = self.candidate()
        client = FakeGitHub(self.commit)
        verified = []
        receipt = self.publish(candidate, client, attestor=lambda path, source: verified.append((path.name, source)))
        self.assertEqual(len(verified), 3)
        self.assertTrue(all(source == self.commit for _, source in verified))
        self.assertTrue(receipt['source_only'])
        self.assertFalse(receipt['native_build'])
        self.assertTrue(receipt['ci_attestation_verified'])
        self.assertEqual(receipt['inputs'], self.lock['inputs'])
        self.assertEqual(receipt['cli_version'], '0.6.25')
        self.assertTrue(receipt['latest_unchanged'])
        self.assertEqual({item[1] for item in client.events if item[0] == 'upload'},
                         {'dockpipe-catalog-linux-amd64.tar.gz', 'source-store-release.json'})
        client.events.clear()
        self.publish(candidate, client, 'resume')
        self.assertEqual(client.events, [])

    def test_missing_attestation_or_failed_consumer_does_not_create_release(self):
        candidate = self.candidate()
        client = FakeGitHub(self.commit)
        def denied(path, source):
            raise ValueError('Unverified CI attestation')
        with self.assertRaises(ValueError):
            self.publish(candidate, client, attestor=denied)
        self.assertEqual(client.events, [])
        proof = self.root / 'consumer-proof.json'
        proof.write_text(json.dumps({'strict_import_recompose': False, 'flatpak_dependency_preflight': True}))
        descriptor = native_store.read_json(candidate)
        descriptor['consumer_proof_sha256'] = native_store.sha256(proof)
        candidate.write_text(json.dumps(descriptor))
        with self.assertRaises(ValueError):
            self.publish(candidate, client)
        self.assertEqual(client.events, [])

    def test_manual_dispatch_wrong_lock_and_development_source_cannot_publish(self):
        candidate = self.candidate()
        client = FakeGitHub(self.commit)
        self.environment['GITHUB_EVENT_NAME'] = 'workflow_dispatch'
        with self.assertRaises(ValueError):
            self.publish(candidate, client)
        self.environment['GITHUB_EVENT_NAME'] = 'push'
        descriptor = native_store.read_json(candidate)
        descriptor['source_state'] = 'development'
        candidate.write_text(json.dumps(descriptor))
        with self.assertRaises(ValueError):
            self.publish(candidate, client)
        self.assertEqual(client.events, [])

    def test_attestation_command_pins_workflow_source_ref_and_hosted_runner(self):
        with patch.object(source_overlay.publish_candidate, 'command') as command:
            source_overlay.verify_attestation(self.root / 'candidate', self.commit)
        arguments = command.call_args.args[0]
        self.assertIn('--deny-self-hosted-runners', arguments)
        self.assertEqual(arguments[arguments.index('--source-digest') + 1], self.commit)
        self.assertEqual(arguments[arguments.index('--source-ref') + 1], 'refs/heads/dev')


if __name__ == '__main__':
    unittest.main()
