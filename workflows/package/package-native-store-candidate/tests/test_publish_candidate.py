import copy
import hashlib
import json
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

import test_native_store as store_tests
from test_native_store import SCRIPTS
sys.path.insert(0, str(SCRIPTS))
import native_store
import publish_candidate


class FakeGitHub:
    def __init__(self, source):
        self.source = source
        self.current = None
        self.events = []
        self.bytes = {}
        self.fail_after_upload_once = False
        self.wrong_public_bytes = False
        self.wrong_dev = False
        self.wrong_tag = False
        self.latest_changed = False

    def latest(self):
        if self.latest_changed and self.current and not self.current['draft']:
            return 99, 'unexpected'
        return 4, 'unchanged-production'

    def reference(self, name):
        if name == 'refs/heads/dev':
            return {'target': {'__typename': 'Commit', 'oid': 'f'*40 if self.wrong_dev else self.source}}
        if self.wrong_tag:
            return {'target': {'__typename': 'Commit', 'oid': 'f'*40}}
        if self.current and not self.current['draft']:
            return {'target': {'__typename': 'Commit', 'oid': self.source}}
        return None

    def find(self, tag):
        return copy.deepcopy(self.current)

    def release(self, identity):
        return copy.deepcopy(self.current)

    def create(self, tag, source):
        self.events.append(('create', tag, source))
        self.current = {'id': 7, 'tag_name': tag, 'target_commitish': source,
                        'prerelease': True, 'draft': True, 'assets': []}

    def upload(self, tag, path):
        if path.name in self.bytes:
            raise AssertionError('Existing bytes were uploaded again')
        self.events.append(('upload', path.name))
        data = path.read_bytes()
        self.bytes[path.name] = data
        self.current['assets'].append({'name': path.name, 'size': len(data),
                                       'digest': 'sha256:' + hashlib.sha256(data).hexdigest(),
                                       'state': 'uploaded'})
        if self.fail_after_upload_once:
            self.fail_after_upload_once = False
            raise ValueError('Simulated ambiguous network completion after upload')

    def publish(self, tag):
        self.events.append(('publish', tag))
        self.current['draft'] = False

    def public_digest(self, url, size):
        name = url.rsplit('/', 1)[1]
        if size != len(self.bytes[name]):
            raise AssertionError('Readback size differs')
        return '0'*64 if self.wrong_public_bytes else hashlib.sha256(self.bytes[name]).hexdigest()


class PublisherTest(unittest.TestCase):
    def setUp(self):
        self.fixture = store_tests.NativeStoreTest(methodName='runTest')
        self.fixture.setUp()
        self.addCleanup(self.fixture.temporary.cleanup)
        self.root = self.fixture.root
        self.source = self.fixture.commit
        self.lock_document = self.fixture.lock()
        self.lock_path = self.fixture.save_lock(self.lock_document)
        self.candidate, self.archive = self.fixture.candidate()
        descriptor = json.loads(self.candidate.read_text())
        descriptor.update(inputs_lock_sha256=native_store.sha256(self.lock_path),
                          source_state='committed-snapshot', checks=publish_candidate.CHECKS,
                          source_archive_sha256=self.lock_document['profiles']['linux-amd64']['inputs']['source']['sha256'])
        self.candidate.write_text(json.dumps(descriptor))
        self.environment = {'GITHUB_REPOSITORY': native_store.REPOSITORY, 'GITHUB_REF': 'refs/heads/dev',
                            'GITHUB_EVENT_NAME': 'push', 'GITHUB_SHA': self.source}
        self.client = FakeGitHub(self.source)

    def publish(self, suffix='first'):
        return publish_candidate.publish([self.candidate], self.lock_path, self.root / suffix,
                                         self.client, self.environment)

    def test_new_release_creates_draft_uploads_exact_assets_and_public_readback(self):
        receipt = self.publish()
        self.assertTrue(receipt['public_asset_readback'])
        self.assertTrue(receipt['latest_unchanged'])
        self.assertEqual(receipt['platforms'], ['linux-amd64'])
        self.assertEqual({event[1] for event in self.client.events if event[0] == 'upload'},
                         {self.archive.name, publish_candidate.METADATA_NAME})
        self.assertEqual(self.client.events[0][0], 'create')
        self.assertEqual(self.client.events[-1][0], 'publish')

    def test_exact_published_candidate_resumes_read_only(self):
        self.publish()
        self.client.events.clear()
        self.assertTrue(self.publish('again')['success'])
        self.assertEqual(self.client.events, [])

    def test_partial_draft_after_ambiguous_upload_resumes_without_replacing(self):
        self.client.fail_after_upload_once = True
        with self.assertRaises(ValueError):
            self.publish()
        self.assertTrue(self.client.current['draft'])
        receipt = self.publish('resume')
        self.assertTrue(receipt['success'])
        uploads = [event[1] for event in self.client.events if event[0] == 'upload']
        self.assertEqual(len(uploads), len(set(uploads)))
        self.assertEqual(len(uploads), 2)

    def test_published_missing_asset_is_never_repaired_in_place(self):
        self.publish()
        self.client.current['assets'].pop()
        self.client.events.clear()
        with self.assertRaises(ValueError):
            self.publish('incomplete')
        self.assertEqual(self.client.events, [])

    def test_existing_changed_or_extra_asset_fails_before_mutation(self):
        self.publish()
        self.client.current['draft'] = True
        self.client.current['assets'][0]['digest'] = 'sha256:' + '0'*64
        self.client.events.clear()
        with self.assertRaises(ValueError):
            self.publish('changed')
        self.assertEqual(self.client.events, [])
        self.client.current['assets'].append({'name': 'unexpected'})
        with self.assertRaises(ValueError):
            self.publish('extra')
        self.assertEqual(self.client.events, [])

    def test_stale_dev_head_or_wrong_tag_fails_before_create(self):
        self.client.wrong_dev = True
        with self.assertRaises(ValueError):
            self.publish('stale')
        self.assertEqual(self.client.events, [])
        self.client.wrong_dev = False
        self.client.wrong_tag = True
        with self.assertRaises(ValueError):
            self.publish('wrong-tag')
        self.assertEqual(self.client.events, [])

    def test_only_accepted_dev_push_can_publish(self):
        self.environment['GITHUB_EVENT_NAME'] = 'workflow_dispatch'
        with self.assertRaises(ValueError):
            self.publish()
        self.assertEqual(self.client.events, [])

    def test_public_readback_failure_has_no_success_receipt(self):
        self.client.wrong_public_bytes = True
        with self.assertRaises(ValueError):
            self.publish()
        self.assertFalse((self.root / 'first/publication-receipt.json').exists())

    def test_latest_change_is_reported_without_rollback_or_replacement(self):
        self.client.latest_changed = True
        with self.assertRaises(ValueError):
            self.publish()
        self.assertFalse((self.root / 'first/publication-receipt.json').exists())
        self.assertTrue(all(event[0] in {'create', 'upload', 'publish'} for event in self.client.events))

    def test_partial_platform_selection_must_be_explicit_in_lock(self):
        self.lock_document['accepted_platforms'].append('windows-amd64')
        self.lock_path.write_text(json.dumps(self.lock_document))
        with self.assertRaises(ValueError):
            self.publish()
        self.assertEqual(self.client.events, [])

    def test_candidate_must_bind_exact_lock_and_committed_source(self):
        document = json.loads(self.candidate.read_text())
        document['source_state'] = 'development'
        self.candidate.write_text(json.dumps(document))
        with self.assertRaises(ValueError):
            self.publish()
        self.assertEqual(self.client.events, [])

    def test_public_reader_drops_credentials_and_checks_bounded_bytes(self):
        data = b'public fixture'
        observed = []
        def command(arguments, environment=None):
            observed.append(arguments)
            self.assertNotIn('GH_TOKEN', environment)
            self.assertNotIn('GITHUB_TOKEN', environment)
            self.assertNotIn('--header', arguments)
            Path(arguments[arguments.index('--output') + 1]).write_bytes(data)
            return 'https://release-assets.githubusercontent.com/fixture'
        with patch.dict(os.environ, {'GH_TOKEN': 'fixture', 'GITHUB_TOKEN': 'fixture'}):
            with patch.object(publish_candidate, 'command', side_effect=command):
                actual = publish_candidate.GitHub().public_digest(
                    'https://github.com/' + native_store.REPOSITORY + '/releases/download/test/fixture', len(data))
        self.assertEqual(actual, hashlib.sha256(data).hexdigest())
        self.assertIn('--max-filesize', observed[0])

    def test_latest_404_is_absence_but_authentication_error_fails_closed(self):
        client = publish_candidate.GitHub()
        with patch.object(client, 'api', side_effect=ValueError('failed; HTTP 404)')):
            self.assertIsNone(client.latest())
        with patch.object(client, 'api', side_effect=ValueError('failed; HTTP 403)')):
            with self.assertRaises(ValueError):
                client.latest()

    def test_mutation_commands_never_clobber_force_delete_or_promote_latest(self):
        operations = []
        with patch.object(publish_candidate, 'command', side_effect=lambda args: operations.append(args) or ''):
            client = publish_candidate.GitHub()
            client.create('candidate', self.source)
            client.upload('candidate', self.archive)
            client.publish('candidate')
        flattened = [item for operation in operations for item in operation]
        for forbidden in ('--clobber', '--force', 'delete', '--latest', '--latest=true'):
            self.assertNotIn(forbidden, flattened)
        self.assertEqual(flattened.count('--latest=false'), 2)
        for operation in operations:
            self.assertEqual(operation[operation.index('--repo') + 1],
                             'https://github.com/' + native_store.REPOSITORY)

    def test_malformed_release_inventory_fails_closed(self):
        client = publish_candidate.GitHub()
        with patch.object(client, 'api', return_value=[None]):
            with self.assertRaises(ValueError):
                client.find('candidate')

    def test_source_reference_lookup_is_bound_to_public_github(self):
        observed = []
        def command(arguments):
            observed.append(arguments)
            return json.dumps({'data': {'repository': {'ref': None}}})
        with patch.object(publish_candidate, 'command', side_effect=command):
            self.assertIsNone(publish_candidate.GitHub().reference('refs/heads/dev'))
        self.assertEqual(observed[0][:5], ['gh', 'api', '--hostname', 'github.com', 'graphql'])


if __name__ == '__main__':
    unittest.main()
