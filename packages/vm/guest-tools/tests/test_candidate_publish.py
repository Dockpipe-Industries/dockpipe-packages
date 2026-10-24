"""Synthetic GitHub transport tests; no network or publication is performed."""
import copy
import importlib.util
import io
import os
from pathlib import Path
import sys
import unittest
from unittest import mock

import test_candidate_release as fixtures

TOOLING = Path(__file__).resolve().parents[1] / 'tooling'
sys.path.insert(0, str(TOOLING))
specification = importlib.util.spec_from_file_location('guest_publish', TOOLING / 'publish_candidate.py')
publisher = importlib.util.module_from_spec(specification)
specification.loader.exec_module(publisher)


class CandidatePublishTest(unittest.TestCase):
    def setUp(self):
        self.original_latest_release = publisher.latest_release
        self.original_attestation_verifier = publisher.verify_attestations
        attestations = mock.patch.object(publisher, 'verify_attestations')
        self.attestations = attestations.start()
        self.addCleanup(attestations.stop)
        latest = mock.patch.object(publisher, 'latest_release', return_value=(7, 'unchanged-production'))
        self.latest = latest.start()
        self.addCleanup(latest.stop)
        self.fixture = fixtures.CandidateReleaseTest()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.prepared = self.fixture.root / 'prepared'
        publisher.candidate.prepare(self.fixture.inputs, self.prepared, fixtures.SOURCE)
        self.directory = self.prepared / 'assets'
        self.manifest, self.assets, self.data = publisher.local_assets(self.directory, fixtures.SOURCE)
        self.release = {'id': 123, 'tag_name': self.manifest['tag'], 'target_commitish': fixtures.SOURCE,
                        'draft': True, 'prerelease': True,
                        'assets': [{'name': name, 'size': entry['size_bytes'],
                                    'digest': 'sha256:' + entry['sha256']}
                                   for name, entry in self.assets.items()]}

    def environment(self):
        return {'GITHUB_SHA': fixtures.SOURCE, 'GITHUB_EVENT_NAME': 'push',
                'GITHUB_REF': 'refs/heads/dev', 'GITHUB_REPOSITORY': publisher.candidate.REPOSITORY}

    def test_partial_exact_draft_resumes_only_missing_assets(self):
        self.release['assets'] = self.release['assets'][:2]
        result = publisher.missing_draft_assets(self.release, None, self.manifest, self.assets)
        self.assertEqual(set(result), set(self.assets) - {entry['name'] for entry in self.release['assets']})

    def test_conflicting_partial_draft_never_clobbered(self):
        for mutation in ('hash', 'duplicate', 'extra', 'source'):
            with self.subTest(mutation=mutation):
                release = copy.deepcopy(self.release)
                if mutation == 'hash':
                    release['assets'][0]['digest'] = 'sha256:' + '0' * 64
                elif mutation == 'duplicate':
                    release['assets'][1] = release['assets'][0]
                elif mutation == 'extra':
                    release['assets'][0]['name'] = 'unlisted.bin'
                else:
                    release['target_commitish'] = '2' * 40
                with self.assertRaises(ValueError):
                    publisher.missing_draft_assets(release, fixtures.SOURCE, self.manifest, self.assets)

    def test_published_release_requires_complete_exact_tag(self):
        self.release['draft'] = False
        self.assertEqual(publisher.missing_draft_assets(self.release, fixtures.SOURCE,
                                                       self.manifest, self.assets), [])
        with self.assertRaises(ValueError):
            publisher.missing_draft_assets(self.release, None, self.manifest, self.assets)
        self.release['assets'].pop()
        with self.assertRaises(ValueError):
            publisher.missing_draft_assets(self.release, fixtures.SOURCE, self.manifest, self.assets)

    def test_pr_and_manual_publication_refused_before_any_transport(self):
        for change in ({'GITHUB_EVENT_NAME': 'pull_request'}, {'GITHUB_EVENT_NAME': 'workflow_dispatch'},
                       {'GITHUB_REF': 'refs/heads/master'}, {'GITHUB_REPOSITORY': 'other/repository'}):
            environment = self.environment() | change
            with self.subTest(change=change), mock.patch.dict(os.environ, environment, clear=True), \
                    mock.patch.object(publisher, 'run') as run, mock.patch.object(publisher, 'api') as api:
                with self.assertRaisesRegex(ValueError, 'accepted exact owning dev push'):
                    publisher.publish(self.directory)
                run.assert_not_called()
                api.assert_not_called()

    def test_resume_revalidates_then_publishes_nonlatest_and_public_readback(self):
        partial = copy.deepcopy(self.release)
        partial['assets'].pop()
        published = copy.deepcopy(self.release)
        published['draft'] = False
        with mock.patch.dict(os.environ, self.environment(), clear=True), \
                mock.patch.object(publisher, 'find_release', return_value=partial), \
                mock.patch.object(publisher, 'tag_target', return_value=fixtures.SOURCE), \
                mock.patch.object(publisher, 'api', side_effect=[self.release, published, published]) as api, \
                mock.patch.object(publisher, 'run') as run, \
                mock.patch.object(publisher, 'public_readback') as readback:
            result = publisher.publish(self.directory)
        self.assertEqual(result['public_readback'], 'passed')
        self.assertEqual(run.call_count, 1)
        command = run.call_args.args[0]
        self.assertEqual(command[:3], ['gh', 'release', 'upload'])
        self.assertNotIn('--clobber', command)
        self.assertEqual(api.call_args_list[1].args[1:],
                         ('PATCH', {'draft': False, 'prerelease': True, 'make_latest': 'false'}))
        readback.assert_called_once()

    def test_new_candidate_creates_draft_then_exact_uploads_and_nonlatest_publish(self):
        empty = copy.deepcopy(self.release)
        empty['assets'] = []
        published = copy.deepcopy(self.release)
        published['draft'] = False
        with mock.patch.dict(os.environ, self.environment(), clear=True), \
                mock.patch.object(publisher, 'find_release', return_value=None), \
                mock.patch.object(publisher, 'tag_target', side_effect=[None, None, fixtures.SOURCE]), \
                mock.patch.object(publisher, 'api', side_effect=[empty, self.release, published, published]) as api, \
                mock.patch.object(publisher, 'run') as run, \
                mock.patch.object(publisher, 'public_readback'):
            publisher.publish(self.directory)
        self.assertEqual(run.call_count, 6)
        creation = api.call_args_list[0].args
        self.assertEqual(creation[1], 'POST')
        self.assertIs(creation[2]['draft'], True)
        self.assertIs(creation[2]['prerelease'], True)
        self.assertEqual(creation[2]['make_latest'], 'false')
        self.assertEqual(creation[2]['target_commitish'], fixtures.SOURCE)
        self.assertTrue(all('--clobber' not in call.args[0] for call in run.call_args_list))

    def test_release_inventory_includes_drafts_and_rejects_ambiguous_or_unbounded_discovery(self):
        first_page = [{'tag_name': 'unrelated'} for _ in range(100)]
        with mock.patch.object(publisher, 'api', side_effect=[first_page, [self.release]]):
            self.assertEqual(publisher.find_release(self.manifest['tag']), self.release)
        with mock.patch.object(publisher, 'api', return_value=[self.release, self.release]):
            with self.assertRaisesRegex(ValueError, 'ambiguous'):
                publisher.find_release(self.manifest['tag'])
        with mock.patch.object(publisher, 'api', return_value=first_page) as api:
            with self.assertRaisesRegex(ValueError, 'bounded discovery'):
                publisher.find_release(self.manifest['tag'])
            self.assertEqual(api.call_count, 20)

    def test_identical_published_candidate_is_read_back_without_mutation(self):
        self.release['draft'] = False
        with mock.patch.dict(os.environ, self.environment(), clear=True), \
                mock.patch.object(publisher, 'find_release', return_value=self.release), \
                mock.patch.object(publisher, 'tag_target', return_value=fixtures.SOURCE), \
                mock.patch.object(publisher, 'api', return_value=self.release) as api, \
                mock.patch.object(publisher, 'run') as run, \
                mock.patch.object(publisher, 'public_readback') as readback:
            publisher.publish(self.directory)
        run.assert_not_called()
        self.assertTrue(all(len(call.args) == 1 for call in api.call_args_list))
        readback.assert_called_once()

    def test_wrong_source_tag_rejected_before_release_mutation(self):
        with mock.patch.dict(os.environ, self.environment(), clear=True), \
                mock.patch.object(publisher, 'find_release', return_value=None), \
                mock.patch.object(publisher, 'tag_target', return_value='2' * 40), \
                mock.patch.object(publisher, 'api') as api:
            with self.assertRaisesRegex(ValueError, 'different source'):
                publisher.publish(self.directory)
        api.assert_not_called()

    def test_public_readback_is_anonymous_and_bounded(self):
        selected = {'proof.bin': {'size_bytes': 3, 'sha256': publisher.candidate.digest(b'abc')}}
        for content in (b'ab', b'abcd', b'abx'):
            with self.subTest(content=content), \
                    mock.patch.object(publisher, 'open_public', return_value=io.BytesIO(content)):
                with self.assertRaises(ValueError):
                    publisher.public_readback(self.manifest, selected)
        with mock.patch.object(publisher, 'open_public', return_value=io.BytesIO(b'abc')) as request:
            publisher.public_readback(self.manifest, selected)
        self.assertTrue(request.call_args.args[0].endswith('/' + self.manifest['tag'] + '/proof.bin'))
        self.assertEqual(request.call_args.kwargs, {})


    def test_attestation_admission_binds_all_six_byte_snapshots_source_and_workflow(self):
        observed = []
        def capture(arguments):
            observed.append((arguments, Path(arguments[3]).read_bytes()))
        with mock.patch.object(publisher, 'run', side_effect=capture):
            self.original_attestation_verifier(self.data, fixtures.SOURCE)
        self.assertEqual(len(observed), 6)
        for arguments, payload in observed:
            self.assertEqual(arguments[:3], ['gh', 'attestation', 'verify'])
            self.assertEqual(payload, self.data[Path(arguments[3]).name])
            self.assertEqual(arguments[arguments.index('--source-digest') + 1], fixtures.SOURCE)
            self.assertEqual(arguments[arguments.index('--source-ref') + 1], 'refs/heads/dev')
            self.assertEqual(arguments[arguments.index('--signer-workflow') + 1],
                             publisher.candidate.REPOSITORY + '/.github/workflows/guest-tools-candidate.yml')
            self.assertIn('--deny-self-hosted-runners', arguments)

    def test_missing_attestation_refuses_before_any_release_operation(self):
        self.attestations.side_effect = ValueError('Attestation missing')
        with mock.patch.dict(os.environ, self.environment(), clear=True), \
                mock.patch.object(publisher, 'find_release') as release, mock.patch.object(publisher, 'api') as api:
            with self.assertRaisesRegex(ValueError, 'Attestation missing'):
                publisher.publish(self.directory)
        release.assert_not_called()
        api.assert_not_called()
        self.latest.assert_not_called()

    def test_public_redirect_downgrade_credentials_and_wrong_hosts_are_rejected(self):
        for url in ('http://github.com/asset', 'https://example.invalid/asset',
                    'https://token@github.com/asset', 'https://github.com:444/asset'):
            with self.subTest(url=url), self.assertRaises(ValueError):
                publisher.PublicRedirect().redirect_request(None, None, 302, '', {}, url)
        for url in ('https://github.com/asset', 'https://release-assets.githubusercontent.com/asset'):
            publisher.validate_public_url(url)
        with mock.patch.object(publisher.urllib.request, 'build_opener') as opener:
            publisher.open_public('https://github.com/asset')
        opener.return_value.open.assert_called_once_with('https://github.com/asset', timeout=60)

    def test_latest_404_is_absence_but_authentication_error_fails_closed(self):
        with mock.patch.object(publisher, 'api', side_effect=ValueError('operation failed; HTTP 404')):
            self.assertIsNone(self.original_latest_release())
        with mock.patch.object(publisher, 'api', side_effect=ValueError('operation failed; HTTP 403')):
            with self.assertRaises(ValueError):
                self.original_latest_release()

    def test_public_readback_has_overall_deadline(self):
        selected = {'proof.bin': {'size_bytes': 3, 'sha256': publisher.candidate.digest(b'abc')}}
        with mock.patch.object(publisher, 'open_public', return_value=io.BytesIO(b'abc')), \
                mock.patch.object(publisher.time, 'monotonic', side_effect=[0, 301]):
            with self.assertRaisesRegex(ValueError, 'deadline'):
                publisher.public_readback(self.manifest, selected)

    def test_latest_must_remain_unchanged_without_rollback(self):
        self.release['draft'] = False
        self.latest.side_effect = [(7, 'unchanged-production'), (8, 'concurrent-change')]
        with mock.patch.dict(os.environ, self.environment(), clear=True), \
                mock.patch.object(publisher, 'find_release', return_value=self.release), \
                mock.patch.object(publisher, 'tag_target', return_value=fixtures.SOURCE), \
                mock.patch.object(publisher, 'api', return_value=self.release), \
                mock.patch.object(publisher, 'run') as mutation, \
                mock.patch.object(publisher, 'public_readback'):
            with self.assertRaisesRegex(ValueError, 'Latest release changed'):
                publisher.publish(self.directory)
        mutation.assert_not_called()

    def test_asset_mutation_after_manifest_generation_is_refused(self):
        (self.directory / publisher.candidate.NAMES['guest_tools']).write_bytes(b'changed')
        with self.assertRaises(ValueError):
            publisher.local_assets(self.directory, fixtures.SOURCE)


if __name__ == '__main__':
    unittest.main()
