import copy
import hashlib
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'assets' / 'scripts'))
from catalog_plan import build_plan, compose_platform
from catalog_transaction import CatalogTransaction, PublicationStopped, StoredObject, fingerprint_inventory


def entry(kind, name, version='1.0.0'):
    return {'name': name, 'version': version,
            'tarball': f'dockpipe-{kind}-{name}-{version}.tar.gz',
            'sha256': 'a' * 64, 'size_bytes': 4}


def store(platform='linux-amd64', resolver=True):
    return {'schema': 1, 'platform': platform, 'packages': {
        'core': {'name': 'core'},
        'resolvers': [entry('resolver', 'qemu')] if resolver else [],
        'workflows': [entry('workflow', 'unrelated')]}}


class CompositionTest(unittest.TestCase):
    def compose(self, metadata=None, baseline=None, policy=None):
        return compose_platform('linux-amd64', baseline or store(), store(),
                                ['resolver/qemu'], policy or {'resolver/qemu': {
                                    'action': 'promote', 'reason': 'Qualified native input'}},
                                {'resolver/qemu': metadata or {}})

    def test_core_excluded_and_unrelated_exact(self):
        baseline = store()
        output, report = self.compose(baseline=baseline)
        self.assertNotIn('core', output['packages'])
        self.assertEqual(output['packages']['workflows'], baseline['packages']['workflows'])
        self.assertTrue(report['core_excluded'])

    def test_requires_resolver_cannot_use_workflow(self):
        baseline = store()
        baseline['packages']['workflows'].append(entry('workflow', 'dorkpipe'))
        with self.assertRaisesRegex(ValueError, 'missing or ambiguous'):
            self.compose({'requires_resolvers': ['dorkpipe']}, baseline)

    def test_implicit_resolver_must_be_declared(self):
        with self.assertRaisesRegex(ValueError, 'undeclared resolver'):
            self.compose({'resolver': 'hidden'})

    def test_platform_contract_retained(self):
        with self.assertRaisesRegex(ValueError, 'host platform'):
            self.compose({'workflow_platforms': ['macos']})

    def test_explicit_retirement_removes_old(self):
        output, report = self.compose(policy={'resolver/qemu': {
            'action': 'retire', 'reason': 'No qualified resolver closure on host'}})
        self.assertEqual(output['packages']['resolvers'], [])
        self.assertEqual(report['decisions'][0]['action'], 'retire')

    def test_missing_policy_rejected(self):
        with self.assertRaisesRegex(ValueError, 'explicit decisions'):
            compose_platform('linux-amd64', store(), store(), ['resolver/qemu'], {}, {})


class FakeProvider:
    def __init__(self):
        self.objects = {}
        self.writes = []
        self.uncertain = False
        self.race = False

    def read(self, key, limit):
        payload = self.objects.get(key)
        if payload is None:
            return None
        if len(payload) > limit:
            raise PublicationStopped('Read capacity exceeded')
        return StoredObject(hashlib.sha256(payload).hexdigest(), len(payload), 'admin-etag',
                            'no-cache' if key == 'packages/latest.json' else '')

    def create(self, key, source, expected):
        if key in self.objects:
            raise PublicationStopped('Conditional create conflict')
        if hashlib.sha256(source).hexdigest() != expected['sha256'] or len(source) != expected['size_bytes']:
            raise PublicationStopped('Source mismatch')
        self.objects[key] = source
        self.writes.append(key)
        if self.uncertain:
            raise PublicationStopped('Unknown outcome')

    def compare_and_swap(self, key, payload, etag):
        if self.race:
            raise PublicationStopped('Conditional pointer conflict')
        self.objects[key] = payload
        self.writes.append(key)


class TransactionTest(unittest.TestCase):
    def setUp(self):
        prefix = 'packages/catalog/candidates/' + '1' * 40 + '/' + '2' * 40
        payload = b'wrapper'
        self.item = {'key': prefix + '/release-manifest.json', 'role': 'release-manifest',
                     'bytes': payload, 'sha256': hashlib.sha256(payload).hexdigest(), 'size_bytes': len(payload)}
        self.plan = {'prefix': prefix, 'pointer_key': 'packages/latest.json',
                     'pointer_bytes': b'new-pointer', 'objects': [self.item],
                     'product_source_sha': '1' * 40, 'orchestration_source_sha': '2' * 40}
        self.provider = FakeProvider()
        self.events = []

    def consumer(self, plan):
        return {'wrapper_sha256': self.item['sha256'],
                'object_inventory_sha256': fingerprint_inventory(plan['objects']),
                'exit_code': 0, 'source_hidden': True}

    def publish(self, consumer=None, public=None):
        transaction = CatalogTransaction(self.provider, public or self.provider.read,
                                         consumer or self.consumer, self.events.append)
        return transaction.publish(self.plan, None, lambda item: item['bytes'])

    def test_fresh_and_resume_do_not_overwrite(self):
        self.assertFalse(self.publish()['resumed'])
        writes = list(self.provider.writes)
        self.assertTrue(self.publish()['resumed'])
        self.assertEqual(writes, self.provider.writes)

    def test_unknown_write_outcome_stops_before_pointer(self):
        self.provider.uncertain = True
        with self.assertRaisesRegex(PublicationStopped, 'Unknown outcome'):
            self.publish()
        self.assertNotIn('packages/latest.json', self.provider.objects)
        self.assertEqual(len(self.provider.writes), 1)

    def test_immutable_collision_rejected(self):
        self.provider.objects[self.item['key']] = b'changed'
        with self.assertRaisesRegex(PublicationStopped, 'collision'):
            self.publish()
        self.assertEqual(self.provider.writes, [])

    def test_cas_conflict_not_retried(self):
        self.provider.race = True
        with self.assertRaisesRegex(PublicationStopped, 'pointer conflict'):
            self.publish()
        self.assertNotIn('packages/latest.json', self.provider.objects)

    def test_consumer_failure_never_changes_pointer(self):
        def failed(plan):
            receipt = self.consumer(plan)
            receipt['exit_code'] = 1
            return receipt
        with self.assertRaisesRegex(PublicationStopped, 'consumer receipt'):
            self.publish(consumer=failed)
        self.assertNotIn('packages/latest.json', self.provider.objects)

    def test_cdn_baseline_disagreement_stops_before_write(self):
        def public(key, limit):
            return StoredObject('a' * 64, 1, 'cdn-etag')
        with self.assertRaisesRegex(PublicationStopped, 'baseline pointer disagree'):
            self.publish(public=public)
        self.assertEqual(self.provider.writes, [])

    def test_runtime_pointer_outside_authority(self):
        self.plan['pointer_key'] = 'packages/runtime/latest.json'
        with self.assertRaisesRegex(PublicationStopped, 'authority'):
            self.publish()


class ConditionalAdapterTest(unittest.TestCase):
    def provider(self):
        from catalog_s3 import S3Provider
        provider = object.__new__(S3Provider)
        self.calls = []
        provider.immutable_prefix = 'packages/catalog/candidates/' + '1' * 40 + '/' + '2' * 40 + '/'
        provider.put = lambda key, source, expected, condition: self.calls.append((key, condition))
        return provider

    def test_create_only_if_absent(self):
        provider = self.provider()
        key = provider.immutable_prefix + 'archive.tar.gz'
        provider.create(key, b'data', {})
        self.assertEqual(self.calls, [(key, ['--if-none-match', '*'])])

    def test_pointer_uses_admin_etag_verbatim(self):
        provider = self.provider()
        provider.compare_and_swap('packages/latest.json', b'data', '"admin-only-etag"')
        self.assertEqual(self.calls[0][1], ['--if-match', '"admin-only-etag"'])

    def test_absent_pointer_requires_conditional_create(self):
        provider = self.provider()
        provider.compare_and_swap('packages/latest.json', b'data', None)
        self.assertEqual(self.calls[0][1], ['--if-none-match', '*'])

    def test_runtime_write_rejected_by_adapter(self):
        provider = self.provider()
        with self.assertRaisesRegex(PublicationStopped, 'authority'):
            provider.compare_and_swap('packages/runtime/latest.json', b'data', None)
        self.assertEqual(self.calls, [])


class PublicTransportTest(unittest.TestCase):
    def test_off_origin_signed_redirect_rejected(self):
        from catalog_public import redirect_target
        with self.assertRaisesRegex(PublicationStopped, 'admitted origin'):
            redirect_target('https://packages.staging.dockpipe.com/packages/latest.json',
                            'https://release-assets.githubusercontent.com/asset?signature=value')

    def test_source_only_tag_rejected(self):
        request = {'product_source_sha': '1' * 40, 'orchestration_source_sha': '2' * 40,
                   'product_tag': 'catalog-candidate-' + '1' * 40, 'channel': 'staging',
                   'public_origin': 'https://packages.staging.dockpipe.com',
                   'exclude_runtime_core': True}
        with self.assertRaisesRegex(ValueError, 'identities'):
            build_plan(request, {}, {}, {})

    def test_store_version_cannot_escape_directory(self):
        from catalog_plan import entries
        manifest = store()
        manifest['packages']['resolvers'][0]['version'] = '../../runtime'
        with self.assertRaisesRegex(ValueError, 'version'):
            entries(manifest)


class ConsumerBoundaryTest(unittest.TestCase):
    def test_actual_invocation_has_no_global_workdir_flag(self):
        import tempfile
        from unittest.mock import patch
        from catalog_consumer import run_cli
        class FinishedProcess:
            returncode = 0
            def poll(self):
                return 0
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch('catalog_consumer.subprocess.Popen', return_value=FinishedProcess()) as launch:
                run_cli(Path('/admitted/dockpipe'), ['package', 'catalog', '--help'], root,
                        {'DOCKPIPE_WORKDIR': str(root)}, 'help')
            self.assertEqual(launch.call_args.args[0],
                             ['/admitted/dockpipe', 'package', 'catalog', '--help'])
            self.assertEqual(launch.call_args.kwargs['cwd'], root)

    def test_fresh_app_store_rejects_masked_missing_qemu(self):
        import tempfile
        from catalog_consumer import verify_installed_closure
        data = b'compiled'
        expected = {}
        for kind, name in [('workflow', 'windows-vm'), ('resolver', 'qemu')]:
            record = entry(kind, name)
            record['sha256'] = hashlib.sha256(data).hexdigest()
            record['size_bytes'] = len(data)
            expected[kind + '/' + name] = record
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            shared = root / 'previous-resolver-case' / 'resolvers'
            shared.mkdir(parents=True)
            (shared / expected['resolver/qemu']['tarball']).write_bytes(data)
            fresh = root / 'fresh-app-case'
            (fresh / 'workflows').mkdir(parents=True)
            (fresh / 'workflows' / expected['workflow/windows-vm']['tarball']).write_bytes(data)
            with self.assertRaisesRegex(PublicationStopped, 'fresh install dependency closure'):
                verify_installed_closure(fresh, 'workflow/windows-vm',
                                         ['workflow/windows-vm', 'resolver/qemu'], expected)
            (fresh / 'resolvers').mkdir()
            (fresh / 'resolvers' / expected['resolver/qemu']['tarball']).write_bytes(data)
            receipt = verify_installed_closure(fresh, 'workflow/windows-vm',
                                               ['workflow/windows-vm', 'resolver/qemu'], expected)
            self.assertEqual(len(receipt['archives']), 2)

    def test_provider_rejects_other_prefix_before_io(self):
        from catalog_s3 import S3Provider
        provider = object.__new__(S3Provider)
        provider.immutable_prefix = 'packages/catalog/candidates/' + '1' * 40 + '/' + '2' * 40 + '/'
        provider.put = lambda *arguments: self.fail('Forbidden I/O')
        for key in ['packages/runtime/latest.json', 'packages/other/archive.tar.gz']:
            with self.assertRaisesRegex(PublicationStopped, 'authority'):
                provider.create(key, b'data', {})


class DeclarationClosureTest(unittest.TestCase):
    def test_exact_resolver_and_transitive_workflow_closure(self):
        from catalog_declarations import dependency_closure
        available = dict.fromkeys(['workflow/app', 'resolver/qemu', 'workflow/helper'])
        metadata = {'workflow/app': {'requires_resolvers': ['qemu']},
                    'resolver/qemu': {'depends': ['helper']}, 'workflow/helper': {}}
        self.assertEqual(dependency_closure('workflow/app', available, metadata), sorted(available))

    def test_required_resolver_rejects_same_named_workflow(self):
        from catalog_declarations import dependency_closure
        with self.assertRaisesRegex(PublicationStopped, 'missing or ambiguous'):
            dependency_closure('workflow/app', {'workflow/app': {}, 'workflow/qemu': {}},
                               {'workflow/app': {'requires_resolvers': ['qemu']}, 'workflow/qemu': {}})

    def test_cycle_is_not_treated_as_preinstalled_success(self):
        from catalog_declarations import dependency_closure
        metadata = {'workflow/a': {'depends': ['b']}, 'workflow/b': {'depends': ['a']}}
        with self.assertRaisesRegex(PublicationStopped, 'cycle'):
            dependency_closure('workflow/a', metadata, metadata)

    def test_unverified_dependency_metadata_rejected(self):
        from catalog_declarations import dependency_closure
        with self.assertRaisesRegex(PublicationStopped, 'hash-verified'):
            dependency_closure('workflow/a', {'workflow/a': {}, 'resolver/qemu': {}},
                               {'workflow/a': {'requires_resolvers': ['qemu']}})


class NativeJobBarrierTest(unittest.TestCase):
    def setUp(self):
        TransactionTest.setUp(self)

    def consumer(self, plan):
        return TransactionTest.consumer(self, plan)

    def test_stage_does_not_change_pointer(self):
        transaction = CatalogTransaction(self.provider, self.provider.read, self.consumer, self.events.append)
        transaction.stage_objects(self.plan, None, lambda item: item['bytes'])
        self.assertNotIn('packages/latest.json', self.provider.objects)

    def test_finalize_cannot_create_disappeared_object(self):
        transaction = CatalogTransaction(self.provider, self.provider.read, self.consumer, self.events.append)
        staged = transaction.stage_objects(self.plan, None, lambda item: item['bytes'])
        self.provider.objects.pop(self.item['key'])
        writes = list(self.provider.writes)
        with self.assertRaisesRegex(PublicationStopped, 'disappeared or changed'):
            transaction.finish(self.plan, None, staged)
        self.assertEqual(writes, self.provider.writes)

    def test_pointer_payload_cannot_change_between_jobs(self):
        transaction = CatalogTransaction(self.provider, self.provider.read, self.consumer, self.events.append)
        staged = transaction.stage_objects(self.plan, None, lambda item: item['bytes'])
        self.plan['pointer_bytes'] = b'different'
        with self.assertRaisesRegex(PublicationStopped, 'inventory differs'):
            transaction.finish(self.plan, None, staged)
        self.assertNotIn('packages/latest.json', self.provider.objects)


class SealedJobPlanTest(unittest.TestCase):
    def test_exact_plan_roundtrip_and_no_clobber(self):
        import tempfile
        from catalog_bundle import seal, load
        fixture = TransactionTest()
        fixture.setUp()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'plan'
            receipt = seal(fixture.plan, root)
            loaded = load(root, receipt['plan_sha256'])
            self.assertEqual(loaded['pointer_bytes'], fixture.plan['pointer_bytes'])
            self.assertEqual(loaded['objects'][0]['bytes'], fixture.item['bytes'])
            with self.assertRaisesRegex(PublicationStopped, 'fresh'):
                seal(fixture.plan, root)

    def test_generated_payload_change_fails_before_provider(self):
        import tempfile
        from catalog_bundle import seal, load
        fixture = TransactionTest()
        fixture.setUp()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'plan'
            receipt = seal(fixture.plan, root)
            next((root / 'payloads').iterdir()).write_bytes(b'changed')
            with self.assertRaisesRegex(PublicationStopped, 'differs'):
                load(root, receipt['plan_sha256'])


class NativeArtifactGateTest(unittest.TestCase):
    def setUp(self):
        from catalog_consumer_gate import REPOSITORY, WORKFLOW, PLATFORMS
        fixture = TransactionTest()
        fixture.setUp()
        self.plan = fixture.plan
        self.environment = {'GITHUB_RUN_ID': '123', 'GITHUB_RUN_ATTEMPT': '1',
                            'GITHUB_REPOSITORY': REPOSITORY, 'GITHUB_EVENT_NAME': 'workflow_dispatch',
                            'GITHUB_REF': 'refs/heads/staging', 'GITHUB_SHA': '2' * 40}
        self.plan['reports'] = {platform: {'decisions': []} for platform in PLATFORMS}
        self.run = {'status': 'in_progress', 'id': 123, 'run_attempt': 1, 'head_sha': '2' * 40, 'head_branch': 'staging',
                    'event': 'workflow_dispatch', 'path': WORKFLOW,
                    'repository': {'full_name': REPOSITORY}}
        self.jobs = {'total_count': 3, 'jobs': [
            {'name': 'consumer (' + platform + ')', 'status': 'completed', 'conclusion': 'success',
             'run_id': 123, 'head_sha': '2' * 40} for platform in PLATFORMS]}
        self.receipts = {platform: {**fixture.consumer(self.plan), 'repository': REPOSITORY,
                                    'run_id': 123, 'run_attempt': 1,
                                    'orchestration_source_sha': '2' * 40,
                                    'platform': platform, 'cli_sha256': 'a' * 64,
                                    'cli_version_output': '0.6.28', 'VM_operations': False,
                                    'installed': []} for platform in PLATFORMS}
        self.cli = {platform: {'sha256': 'a' * 64, 'version': '0.6.28'} for platform in PLATFORMS}
        self.manifests = {platform: {'schema': 1, 'platform': platform,
                                    'packages': {'resolvers': [], 'workflows': []}}
                          for platform in PLATFORMS}
        self.closures = {platform: {} for platform in PLATFORMS}

    def admit(self):
        from catalog_consumer_gate import admit_native_receipts
        class FakeAPI:
            def __init__(self, run, jobs):
                self.run = run
                self.jobs = jobs
            def get(self, endpoint):
                return self.jobs if '/jobs?' in endpoint else self.run
        return admit_native_receipts(self.plan, self.receipts, self.cli, self.manifests,
                                     self.closures, self.environment, FakeAPI(self.run, self.jobs))

    def test_three_real_job_bindings_required(self):
        result = self.admit()
        self.assertEqual(len(result['native_receipt_sha256']), 3)

    def test_handwritten_success_receipt_cannot_replace_failed_job(self):
        self.jobs['jobs'][0]['conclusion'] = 'failure'
        with self.assertRaisesRegex(PublicationStopped, 'did not pass'):
            self.admit()

    def test_foreign_run_receipt_rejected(self):
        next(iter(self.receipts.values()))['run_id'] = 122
        with self.assertRaisesRegex(PublicationStopped, 'artifact binding'):
            self.admit()

    def test_missing_native_job_not_treated_as_success(self):
        self.jobs['jobs'].pop()
        self.jobs['total_count'] = 2
        with self.assertRaisesRegex(PublicationStopped, 'incomplete'):
            self.admit()

    def test_cli_pin_mismatch_rejected(self):
        next(iter(self.receipts.values()))['cli_sha256'] = 'b' * 64
        with self.assertRaisesRegex(PublicationStopped, 'artifact binding'):
            self.admit()


class PayloadOrderTest(unittest.TestCase):
    def test_actual_staging_uploads_all_archives_before_manifests_and_wrapper(self):
        from catalog_plan import BASELINE_PLATFORMS, NATIVE_PLATFORMS
        baselines = {platform: store(platform) for platform in BASELINE_PLATFORMS}
        candidates = {}
        metadata = {}
        policy = {}
        digest = hashlib.sha256(b'data').hexdigest()
        for platform, baseline in baselines.items():
            for group in ['resolvers', 'workflows']:
                for record in baseline['packages'][group]:
                    record['sha256'] = digest
            action = 'promote' if platform in NATIVE_PLATFORMS else 'retain-baseline'
            policy[platform] = {'resolver/qemu': {'action': action, 'reason': 'Explicit test decision'}}
            if platform in NATIVE_PLATFORMS:
                candidates[platform] = {'schema': 1, 'platform': platform,
                                        'packages': {'resolvers': baseline['packages']['resolvers'], 'workflows': []}}
                metadata[platform] = {'resolver/qemu': {}}
        request = {'product_source_sha': '1' * 40, 'orchestration_source_sha': '2' * 40,
                   'product_tag': 'catalog-native-candidate-' + '1' * 40, 'channel': 'staging',
                   'public_origin': 'https://packages.staging.dockpipe.com',
                   'exclude_runtime_core': True, 'owned': ['resolver/qemu'], 'policy': policy}
        plan = build_plan(request, baselines, candidates, metadata)
        provider = FakeProvider()
        transaction = CatalogTransaction(provider, provider.read, lambda value: self.fail('Consumer not yet run'),
                                         lambda value: None)
        staged = transaction.stage_objects(plan, None, lambda item: item.get('bytes', b'data'))
        roles = {item['key']: item['role'] for item in plan['objects']}
        observed = [roles[key] for key in provider.writes]
        self.assertEqual(observed, sorted(observed, key=lambda role: {
            'archive': 0, 'store-manifest': 1, 'release-manifest': 2}[role]))
        self.assertEqual(observed[-1], 'release-manifest')
        self.assertNotIn('packages/latest.json', provider.objects)
        self.assertEqual(staged['object_inventory_sha256'], fingerprint_inventory(plan['objects']))


if __name__ == '__main__':
    unittest.main()
