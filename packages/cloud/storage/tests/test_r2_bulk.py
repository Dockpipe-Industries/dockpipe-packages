"""Offline bulk transfer contracts; fake remote stores real uploaded bytes."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'resolvers/r2/dockpipe.cloudflare.r2bulk/assets/scripts/r2-bulk.py'
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
        for content in (b'single fixture', b'x' * (8 * 1024 * 1024 + 17)):
            with self.subTest(size=len(content)):
                self.entries = []
                self.add('object', content)
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
                if len(content) < config.multipart_threshold:
                    etag = hashlib.md5(content).hexdigest()
                    stub.add_response('put_object', {'ETag': '"' + etag + '"'},
                                      {**common, 'Body': ANY, 'Metadata': {'sha256': digest}})
                else:
                    stub.add_response('create_multipart_upload', {'UploadId': 'fixture-upload'},
                                      {**common, 'Metadata': {'sha256': digest}})
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
                                                  'Metadata': {'sha256': digest}, 'ETag': '"' + etag + '"'}, common)
                with stub, upload.locked():
                    upload.transfer_one(client, config, upload.objects[0])
                stub.assert_no_pending_responses()
                (self.root / 'state.json').unlink()

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
