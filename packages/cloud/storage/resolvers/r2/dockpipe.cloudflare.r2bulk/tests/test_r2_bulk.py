"""Offline bulk transfer contracts; fake remote stores real uploaded bytes."""
import errno
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'assets/scripts/r2-bulk.py'
spec = importlib.util.spec_from_file_location('r2_bulk', SCRIPT)
bulk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bulk)


class Missing(Exception):
    response = {'Error': {'Code': '404'}}


class FakeS3:
    def __init__(self):
        self.remote = {}
        self.uploads = []
        self.fail_key = None
        self.corrupt_etag = False
        self.active = 0
        self.maximum_active = 0
        self.mutex = threading.Lock()

    def head_object(self, Bucket, Key):
        if Key not in self.remote:
            raise Missing()
        return self.remote[Key].copy()

    def upload_file(self, source, bucket, key, ExtraArgs, Config):
        with self.mutex:
            self.active += 1
            self.maximum_active = max(self.maximum_active, self.active)
        try:
            if key == self.fail_key:
                raise RuntimeError('interrupted fixture')
            time.sleep(0.005)
            content = Path(source).read_bytes()
            part_size = Config if isinstance(Config, int) else 16 * 1024 * 1024
            parts = [hashlib.md5(content[i:i + part_size]).digest()
                     for i in range(0, len(content), part_size)]
            if len(content) >= part_size:
                etag = hashlib.md5(b''.join(parts)).hexdigest() + '-' + str(len(parts))
            else:
                etag = hashlib.md5(content).hexdigest()
            if self.corrupt_etag:
                etag = 'corrupt'
            self.remote[key] = {'ContentLength': len(content), 'Metadata': ExtraArgs['Metadata'], 'ETag': '"' + etag + '"'}
            for header in ('ContentType', 'CacheControl'):
                if header in ExtraArgs:
                    self.remote[key][header] = ExtraArgs[header]
            self.uploads.append(key)
        finally:
            with self.mutex:
                self.active -= 1


class BulkTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.entries = []
        self.environment = {'R2_ENDPOINT_URL': 'https://example.r2.cloudflarestorage.com',
                            'R2_BUCKET': 'fixture-bucket', 'R2_BULK_MANIFEST': str(self.root / 'manifest.json'),
                            'R2_BULK_STATE': str(self.root / 'state.json'), 'R2_BULK_OBJECT_WORKERS': '1'}

    def add(self, key, content=b'fixture', **extra):
        source = self.root / ('source-' + str(len(self.entries)))
        source.write_bytes(content)
        self.entries.append({'source': str(source), 'key': key, 'size': len(content),
                             'sha256': hashlib.sha256(content).hexdigest(), **extra})

    def uploader(self):
        Path(self.environment['R2_BULK_MANIFEST']).write_text(json.dumps({'schema': 1, 'objects': self.entries}))
        return bulk.BulkUpload(self.environment)

    def run_upload(self, client):
        upload = self.uploader()
        with upload.locked():
            upload.transfer(client, None)
            upload.verify(client)
        return upload

    def test_missing_upload_resume_and_marker_last(self):
        self.add('complete', publish_last=True)
        self.add('payload')
        client = FakeS3()
        self.run_upload(client)
        self.assertEqual(client.uploads, ['payload', 'complete'])
        self.run_upload(client)
        self.assertEqual(client.uploads, ['payload', 'complete'])
        self.assertEqual(json.loads((self.root / 'state.json').read_text())['phase'], 'verified-head')

    def test_interruption_reuses_completed_objects_and_blocks_marker(self):
        self.add('a')
        self.add('b')
        self.add('complete', publish_last=True)
        client = FakeS3()
        client.fail_key = 'b'
        with self.assertRaises(RuntimeError):
            self.run_upload(client)
        self.assertEqual(client.uploads, ['a'])
        self.assertIn('a', json.loads((self.root / 'state.json').read_text())['objects'])
        client.fail_key = None
        self.run_upload(client)
        self.assertEqual(client.uploads, ['a', 'b', 'complete'])

    def test_standalone_verify_rejects_metadata_with_bad_etag(self):
        self.add('a')
        client = FakeS3()
        client.remote['a'] = {'ContentLength': 7,
                              'Metadata': {'sha256': self.entries[0]['sha256']},
                              'ETag': '"corrupt"'}
        upload = self.uploader()
        with upload.locked():
            with self.assertRaises(bulk.BulkError):
                upload.verify(client)
        self.assertNotEqual(upload.state['phase'], 'verified-head')
        self.assertNotIn('a', upload.state['objects'])

    def test_existing_metadata_is_reconciled_against_local_etag(self):
        self.add('a')
        client = FakeS3()
        client.remote['a'] = {'ContentLength': 7,
                              'Metadata': {'sha256': self.entries[0]['sha256']},
                              'ETag': '"' + hashlib.md5(b'fixture').hexdigest() + '"'}
        self.run_upload(client)
        self.assertEqual(client.uploads, [])
        receipt = json.loads((self.root / 'state.json').read_text())['objects']['a']
        self.assertEqual(receipt['verification'], 'head-etag-and-local-sha256')

    def test_unknown_existing_and_remote_mismatch_never_overwritten(self):
        self.add('a')
        for head in ({'ContentLength': 7, 'Metadata': {}},
                     {'ContentLength': 8, 'Metadata': {}},
                     {'ContentLength': 7, 'Metadata': {'sha256': '0' * 64}}):
            client = FakeS3()
            client.remote['a'] = head
            with self.assertRaises(bulk.BulkError):
                self.run_upload(client)
            self.assertEqual(client.uploads, [])

    def test_trusted_receipt_requires_exact_destination_and_current_etag(self):
        self.add('a')
        receipt = {'schema': 1, 'endpoint': self.environment['R2_ENDPOINT_URL'], 'bucket': 'fixture-bucket',
                   'objects': [{**self.entries[0], 'verification': 'full-get-sha256', 'etag': '"old"'}]}
        path = self.root / 'receipts.json'
        self.environment['R2_BULK_TRUSTED_RECEIPTS'] = str(path)
        path.write_text(json.dumps(receipt))
        client = FakeS3()
        client.remote['a'] = {'ContentLength': 7, 'Metadata': {}, 'ETag': '"old"'}
        self.run_upload(client)
        self.assertEqual(client.uploads, [])
        client.remote['a']['ETag'] = '"changed"'
        with self.assertRaises(bulk.BulkError):
            self.run_upload(client)
        receipt['bucket'] = 'another-bucket'
        path.write_text(json.dumps(receipt))
        with self.assertRaises(bulk.BulkError):
            self.uploader()

    def test_corrupt_upload_etag_rejected_before_marker(self):
        self.add('a')
        self.add('complete', publish_last=True)
        client = FakeS3()
        client.corrupt_etag = True
        with self.assertRaises(bulk.BulkError):
            self.run_upload(client)
        self.assertNotIn('complete', client.uploads)
        with self.assertRaises(bulk.BulkError):
            self.run_upload(client)
        self.assertNotIn('complete', client.uploads)

    def test_local_hash_mismatch_prevents_upload(self):
        self.add('a')
        self.entries[0]['sha256'] = '0' * 64
        client = FakeS3()
        with self.assertRaises(bulk.BulkError):
            self.run_upload(client)
        self.assertEqual(client.uploads, [])

    def test_duplicate_keys_traversal_symlinks_and_state_collision(self):
        self.add('a')
        original = self.entries[0].copy()
        for change in ({'key': '../escape'}, {'source': '../escape'},
                       {'source': str(self.root / 'link')}):
            (self.root / 'link').unlink(missing_ok=True)
            (self.root / 'link').symlink_to(original['source'])
            self.entries = [{**original, **change}]
            with self.assertRaises(bulk.BulkError):
                self.uploader()
        self.entries = [original, original]
        with self.assertRaises(bulk.BulkError):
            self.uploader()
        self.entries = [original]
        self.environment['R2_BULK_STATE'] = original['source']
        with self.assertRaises(bulk.BulkError):
            self.uploader()

    def test_state_destination_binding_and_exclusive_lock(self):
        self.add('a')
        upload = self.uploader()
        other = self.uploader()
        with upload.locked():
            upload.save()
            with self.assertRaises(bulk.BulkError):
                with other.locked():
                    pass
        self.environment['R2_BUCKET'] = 'different-bucket'
        with self.assertRaises(bulk.BulkError):
            with self.uploader().locked():
                pass

    def test_concurrency_and_combined_request_bounds(self):
        for number in range(10):
            self.add('key-' + str(number))
        self.environment['R2_BULK_OBJECT_WORKERS'] = '3'
        client = FakeS3()
        self.run_upload(client)
        self.assertLessEqual(client.maximum_active, 3)
        self.assertGreater(client.maximum_active, 1)
        self.environment['R2_BULK_MULTIPART_WORKERS'] = '2'
        with self.assertRaises(bulk.BulkError):
            self.uploader()

    def test_real_sdk_single_and_multipart_request_contracts(self):
        try:
            from botocore.stub import ANY, Stubber
        except ImportError:
            self.skipTest('Pinned SDK environment is not installed')
        from unittest.mock import patch
        cases = [
            (content, metadata)
            for content in (b'single fixture', b'x' * (8 * 1024 * 1024 + 17))
            for metadata in ({}, {'content_type': 'application/octet-stream',
                                  'cache_control': 'public, max-age=31536000, immutable'})
        ]
        for content, metadata in cases:
            with self.subTest(size=len(content), metadata=metadata):
                self.entries = []
                self.add('object', content, **metadata)
                self.environment['R2_BULK_PART_MIB'] = '8'
                upload = self.uploader()
                with patch.dict('os.environ', {'AWS_ACCESS_KEY_ID': 'fixture', 'AWS_SECRET_ACCESS_KEY': 'fixture'}):
                    client, config = bulk.sdk(upload)
                self.addCleanup(client.close)
                stub = Stubber(client)
                stub.add_client_error('head_object', service_error_code='404', http_status_code=404,
                                      expected_params={'Bucket': 'fixture-bucket', 'Key': 'object'})
                common = {'Bucket': 'fixture-bucket', 'Key': 'object'}
                digest = self.entries[0]['sha256']
                headers = bulk.publishing_headers(self.entries[0])
                if len(content) < config.multipart_threshold:
                    etag = hashlib.md5(content).hexdigest()
                    stub.add_response('put_object', {'ETag': '"' + etag + '"'},
                                      {**common, 'Body': ANY, 'Metadata': {'sha256': digest}, **headers})
                else:
                    stub.add_response('create_multipart_upload', {'UploadId': 'fixture-upload'},
                                      {**common, 'Metadata': {'sha256': digest}, **headers})
                    parts = []
                    part_digests = []
                    for index, offset in enumerate(range(0, len(content), config.multipart_chunksize), 1):
                        part_digest = hashlib.md5(content[offset:offset + config.multipart_chunksize]).digest()
                        part_digests.append(part_digest)
                        part_etag = '"' + part_digest.hex() + '"'
                        stub.add_response('upload_part', {'ETag': part_etag},
                                          {**common, 'Body': ANY, 'UploadId': 'fixture-upload', 'PartNumber': index})
                        parts.append({'ETag': part_etag, 'PartNumber': index})
                    etag = hashlib.md5(b''.join(part_digests)).hexdigest() + '-' + str(len(parts))
                    stub.add_response('complete_multipart_upload', {'ETag': '"' + etag + '"'},
                                      {**common, 'UploadId': 'fixture-upload', 'MultipartUpload': {'Parts': parts}})
                stub.add_response('head_object', {'ContentLength': len(content),
                                                  'Metadata': {'sha256': digest}, 'ETag': '"' + etag + '"', **headers}, common)
                with stub, upload.locked():
                    upload.transfer_one(client, config, upload.objects[0])
                stub.assert_no_pending_responses()
                (self.root / 'state.json').unlink()

    def test_publishing_headers_are_transferred_verified_and_receipted(self):
        self.add('payload', content_type='application/octet-stream',
                 cache_control='public, max-age=31536000, immutable')
        self.add('marker', publish_last=True, content_type='application/json',
                 cache_control='no-cache')
        client = FakeS3()
        upload = self.run_upload(client)
        self.assertEqual(client.uploads, ['payload', 'marker'])
        self.assertEqual(client.remote['payload']['ContentType'], 'application/octet-stream')
        self.assertEqual(client.remote['marker']['CacheControl'], 'no-cache')
        self.assertEqual(upload.state['objects']['payload']['cache_control'],
                         'public, max-age=31536000, immutable')
        self.run_upload(client)
        self.assertEqual(client.uploads, ['payload', 'marker'])

    def test_changed_remote_headers_reject_resume_even_with_durable_receipt(self):
        self.add('payload', content_type='application/json', cache_control='no-cache')
        client = FakeS3()
        self.run_upload(client)
        for header in ('ContentType', 'CacheControl'):
            original = client.remote['payload'].pop(header)
            with self.subTest(header=header):
                with self.assertRaisesRegex(bulk.BulkError, header):
                    self.run_upload(client)
                self.assertEqual(client.uploads, ['payload'])
            client.remote['payload'][header] = original

    def test_existing_header_mismatch_blocks_marker_and_never_overwrites(self):
        self.add('payload', content_type='application/json')
        self.add('marker', publish_last=True)
        client = FakeS3()
        client.remote['payload'] = {
            'ContentLength': 7,
            'Metadata': {'sha256': self.entries[0]['sha256']},
            'ETag': '"' + hashlib.md5(b'fixture').hexdigest() + '"',
            'ContentType': 'application/octet-stream',
        }
        with self.assertRaisesRegex(bulk.BulkError, 'ContentType'):
            self.run_upload(client)
        self.assertEqual(client.uploads, [])
        self.assertNotIn('marker', client.remote)

    def test_trusted_receipt_cannot_bypass_explicit_headers(self):
        self.add('payload', cache_control='no-cache')
        receipt = {
            'schema': 1,
            'endpoint': self.environment['R2_ENDPOINT_URL'],
            'bucket': 'fixture-bucket',
            'objects': [{**self.entries[0], 'verification': 'full-get-sha256'}],
        }
        path = self.root / 'receipts.json'
        path.write_text(json.dumps(receipt))
        self.environment['R2_BULK_TRUSTED_RECEIPTS'] = str(path)
        client = FakeS3()
        client.remote['payload'] = {'ContentLength': 7, 'Metadata': {}, 'ETag': '"prior"'}
        with self.assertRaisesRegex(bulk.BulkError, 'CacheControl'):
            self.run_upload(client)
        self.assertEqual(client.uploads, [])

    def test_headers_bind_state_and_omission_preserves_legacy_identity(self):
        self.add('payload')
        upload = self.uploader()
        expected_identity = bulk.canonical_hash({
            'endpoint': upload.endpoint,
            'bucket': upload.bucket,
            'objects': [{**self.entries[0], 'publish_last': False}],
            'trusted': {},
        })
        self.assertEqual(upload.identity, expected_identity)
        with upload.locked():
            upload.save()
        self.entries[0]['content_type'] = 'application/json'
        changed = self.uploader()
        self.assertNotEqual(changed.identity, upload.identity)
        with self.assertRaisesRegex(bulk.BulkError, 'another manifest'):
            with changed.locked():
                pass

    def test_invalid_headers_fail_before_remote_operations(self):
        self.add('payload')
        for field in ('content_type', 'cache_control'):
            for value in (None, True, 12, '', ' ', 'text/plain\r\nInjected: value',
                          'text/plain\t', 'café', 'x' * 1025):
                with self.subTest(field=field, value=value):
                    self.entries[0][field] = value
                    with self.assertRaisesRegex(bulk.BulkError, field):
                        self.uploader()
            del self.entries[0][field]

    def invoke_main(self, phase, environment):
        from unittest.mock import patch
        output = io.StringIO()
        errors = io.StringIO()
        with patch.dict('os.environ', environment, clear=True), \
                patch('sys.argv', ['r2-bulk.py', phase]), \
                patch('sys.stdout', output), patch('sys.stderr', errors):
            result = bulk.main()
        return result, output.getvalue(), errors.getvalue()

    def test_skip_all_stages_needs_no_configuration_sdk_or_state(self):
        from unittest.mock import patch
        with patch.object(bulk, 'BulkUpload', side_effect=AssertionError('configuration used')), \
                patch.object(bulk, 'sdk', side_effect=AssertionError('SDK used')):
            for phase in ('preflight', 'transfer', 'verify'):
                result, output, errors = self.invoke_main(phase, {'R2_BULK_SKIP': 'true'})
                self.assertEqual(result, 0, errors)
                self.assertEqual(json.loads(output)['phase'], 'skipped')
                self.assertFalse(json.loads(output)['provider_operations'])
        self.assertFalse((self.root / 'state.json').exists())

    def test_dry_run_is_provider_free_and_same_plan_can_execute(self):
        from unittest.mock import patch
        self.add('payload', content_type='application/octet-stream')
        self.uploader()
        environment = {**self.environment, 'R2_BULK_DRY_RUN': 'true'}
        with patch.object(bulk, 'sdk', side_effect=AssertionError('SDK used')):
            for phase in ('preflight', 'transfer', 'verify'):
                result, output, errors = self.invoke_main(phase, environment)
                self.assertEqual(result, 0, errors)
                self.assertEqual(json.loads(output)['phase'], 'dry-run')
        state = json.loads((self.root / 'state.json').read_text())
        self.assertEqual(state['phase'], 'dry-run')
        self.assertEqual(state['objects'], {})
        client = FakeS3()
        completed = self.run_upload(client)
        self.assertEqual(completed.identity, state['identity'])
        self.assertEqual(completed.state['phase'], 'verified-head')
        self.assertEqual(client.uploads, ['payload'])

    def test_dry_run_validates_inputs_and_flags_are_strict(self):
        result, _, errors = self.invoke_main('preflight', {'R2_BULK_DRY_RUN': 'true'})
        self.assertEqual(result, 1)
        self.assertIn('R2_ENDPOINT_URL', errors)
        for flag in ('R2_BULK_SKIP', 'R2_BULK_DRY_RUN'):
            for value in ('1', 'TRUE', '', 'yes'):
                result, _, errors = self.invoke_main('transfer', {flag: value})
                self.assertEqual(result, 1)
                self.assertIn(flag, errors)

    def test_real_sdk_provider_failure_does_not_upload_marker(self):
        try:
            from botocore.exceptions import ClientError
            from botocore.stub import Stubber
        except ImportError:
            self.skipTest('Pinned SDK environment is not installed')
        from unittest.mock import patch
        self.add('payload', cache_control='public, max-age=31536000, immutable')
        self.add('marker', publish_last=True)
        upload = self.uploader()
        with patch.dict('os.environ', {'AWS_ACCESS_KEY_ID': 'fixture',
                                      'AWS_SECRET_ACCESS_KEY': 'fixture'}):
            client, config = bulk.sdk(upload)
        self.addCleanup(client.close)
        stub = Stubber(client)
        stub.add_client_error('head_object', service_error_code='AccessDenied', http_status_code=403,
                              expected_params={'Bucket': 'fixture-bucket', 'Key': 'payload'})
        with stub, upload.locked():
            with self.assertRaises(ClientError):
                upload.transfer(client, config)
        stub.assert_no_pending_responses()
        self.assertEqual(upload.state['objects'], {})
        self.assertEqual(upload.state['phase'], 'transferring')

    def test_windows_lock_uses_one_fixed_byte_and_releases_it(self):
        from unittest.mock import Mock, call, patch
        calls = Mock()
        windows_lock = SimpleNamespace(LK_NBLCK=1, LK_UNLCK=0, locking=calls)
        lock_path = self.root / 'portable.lock'
        with patch.object(bulk, 'WINDOWS', True), \
                patch.dict('sys.modules', {'msvcrt': windows_lock}):
            with lock_path.open('a+b') as lock:
                descriptor = lock.fileno()
                with bulk.exclusive_state_lock(lock):
                    self.assertEqual(lock.tell(), 0)
                    lock.seek(1)
                self.assertEqual(lock.tell(), 0)
        self.assertEqual(lock_path.read_bytes(), b'\0')
        self.assertEqual(calls.call_args_list, [call(descriptor, 1, 1), call(descriptor, 0, 1)])

    def test_windows_lock_contention_rejects_without_entering_or_unlocking(self):
        from unittest.mock import Mock, patch
        calls = Mock(side_effect=OSError(errno.EACCES, 'locked fixture'))
        windows_lock = SimpleNamespace(LK_NBLCK=1, LK_UNLCK=0, locking=calls)
        with patch.object(bulk, 'WINDOWS', True), \
                patch.dict('sys.modules', {'msvcrt': windows_lock}):
            with (self.root / 'portable.lock').open('a+b') as lock:
                with self.assertRaisesRegex(bulk.BulkError, 'Another bulk process'):
                    with bulk.exclusive_state_lock(lock):
                        self.fail('Contended lock entered')
        self.assertEqual(calls.call_count, 1)

    def test_windows_unlocks_when_state_body_fails(self):
        from unittest.mock import Mock, patch
        calls = Mock()
        windows_lock = SimpleNamespace(LK_NBLCK=1, LK_UNLCK=0, locking=calls)
        with patch.object(bulk, 'WINDOWS', True), \
                patch.dict('sys.modules', {'msvcrt': windows_lock}):
            with (self.root / 'portable.lock').open('a+b') as lock:
                with self.assertRaisesRegex(RuntimeError, 'fixture failure'):
                    with bulk.exclusive_state_lock(lock):
                        raise RuntimeError('fixture failure')
        self.assertEqual(calls.call_count, 2)
        self.assertEqual(calls.call_args.args[1:], (0, 1))

    def test_windows_directory_durability_is_explicit_and_never_claims_fsync(self):
        from unittest.mock import patch
        self.add('payload')
        upload = self.uploader()
        with patch.object(bulk, 'WINDOWS', True):
            with patch.object(bulk.os, 'open') as directory_open:
                bulk.fsync_directory(self.root)
                directory_open.assert_not_called()
            upload.save()
        state = json.loads((self.root / 'state.json').read_text())
        self.assertEqual(state['durability'], 'file-fsync-and-atomic-replace')
        with patch.object(bulk, 'WINDOWS', False):
            self.assertEqual(bulk.state_durability(), 'file-and-directory-fsync')

    def test_real_sdk_is_explicit_and_bounded(self):
        try:
            import boto3
        except ImportError:
            self.skipTest('Pinned SDK environment is not installed')
        self.add('a')
        from unittest.mock import patch
        with patch.dict('os.environ', {'AWS_ACCESS_KEY_ID': 'fixture', 'AWS_SECRET_ACCESS_KEY': 'fixture'}):
            client, config = bulk.sdk(self.uploader())
        self.addCleanup(client.close)
        self.assertEqual(client.meta.config.request_checksum_calculation, 'when_required')
        self.assertEqual(client.meta.config.max_pool_connections, 1)
        self.assertEqual(config.max_concurrency, 1)
        self.assertEqual(config.multipart_chunksize, 16 * 1024 * 1024)
        self.assertEqual(config.preferred_transfer_client, 'classic')


if __name__ == '__main__':
    unittest.main()
