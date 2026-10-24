import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1] / 'assets/scripts'
sys.path.insert(0, str(SCRIPTS))
from catalog_driver import admit_aws_pin, destination_from_environment, request_file
from catalog_plan import json_bytes
from catalog_runtime_gate import admit_runtime, vmimage_digest
from catalog_transaction import PublicationStopped


class FrontendAdmissionTest(unittest.TestCase):
    def test_official_dependency_is_exact_and_source_bound(self):
        source_root = Path(__file__).resolve().parents[4]
        descriptor_path = source_root / 'release/catalog-promotion/aws-cli-2.32.9.json'
        descriptor = json.loads(descriptor_path.read_bytes())
        self.assertEqual(admit_aws_pin(descriptor['archive'], source_root), descriptor)
        altered = dict(descriptor['archive'], sha256='0' * 64)
        with self.assertRaisesRegex(PublicationStopped, 'source-bound'):
            admit_aws_pin(altered, source_root)

    def test_destination_has_references_without_literal_operator_values(self):
        values = {'endpoint': 'https://' + 'a' * 32 + '.r2.cloudflarestorage.com',
                  'bucket': 'fixture-bucket'}
        declaration = {
            'endpoint_reference': 'R2_ENDPOINT_URL',
            'bucket_reference': 'DOCKPIPE_RELEASE_BUCKET',
            'target_sha256': hashlib.sha256(json_bytes(values)).hexdigest(),
        }
        with patch.dict('os.environ', {'R2_ENDPOINT_URL': values['endpoint'],
                                      'DOCKPIPE_RELEASE_BUCKET': values['bucket']}, clear=True):
            self.assertEqual(destination_from_environment(declaration), values)
        with patch.dict('os.environ', {'R2_ENDPOINT_URL': values['endpoint'],
                                      'DOCKPIPE_RELEASE_BUCKET': 'different'}, clear=True):
            with self.assertRaisesRegex(PublicationStopped, 'target digest'):
                destination_from_environment(declaration)
        with self.assertRaisesRegex(PublicationStopped, 'references'):
            destination_from_environment(dict(declaration, endpoint=values['endpoint']))

    def test_wrong_reviewed_request_digest_fails_before_parsing(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'request.json'
            source.write_text('{malformed')
            with self.assertRaisesRegex(PublicationStopped, 'reviewed source bytes'):
                request_file(source, '0' * 64)

    def test_runtime_below_darwin_floor_stops_before_package_or_provider_io(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            document = {'version': '0.6.27', 'manifest': 'irrelevant.json'}
            source = root / 'pointer-source.json'
            source.write_bytes(json_bytes(document))
            pin = {'pointer': {
                'url': 'https://packages.staging.dockpipe.com/packages/runtime/latest.json',
                'sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                'size_bytes': source.stat().st_size,
            }}
            with patch('catalog_runtime_gate.acquire_baseline', return_value=source):
                with self.assertRaisesRegex(PublicationStopped, 'Darwin runtime floor'):
                    admit_runtime(pin, {}, {}, {}, None, root / 'readiness', None)

    def test_effective_script_not_gzip_container_identity_is_admitted(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = b'#!/bin/bash\nverified-runtime-script\n'
            archives = []
            for index, timestamp in enumerate([0, 100]):
                archive = root / (str(index) + '.tar.gz')
                with tarfile.open(archive, 'w:gz') as output:
                    member = tarfile.TarInfo('core/assets/scripts/vmimage-run.sh')
                    member.mtime = timestamp
                    member.size = len(script)
                    output.addfile(member, io.BytesIO(script))
                archives.append(archive)
            self.assertNotEqual(archives[0].read_bytes(), archives[1].read_bytes())
            self.assertEqual(vmimage_digest(archives[0]), vmimage_digest(archives[1]))

    def test_duplicate_effective_script_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / 'core.tar.gz'
            with tarfile.open(archive, 'w:gz') as output:
                for _ in range(2):
                    member = tarfile.TarInfo('core/assets/scripts/vmimage-run.sh')
                    member.size = 1
                    output.addfile(member, io.BytesIO(b'x'))
            with self.assertRaisesRegex(PublicationStopped, 'duplicated'):
                vmimage_digest(archive)


if __name__ == '__main__':
    unittest.main()
