import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'assets/scripts'))
from catalog_consumer_gate import REPOSITORY
from catalog_transaction import PublicationStopped
from catalog_viewer import WORKFLOW, admit_distribution_review, admit_viewer


class ViewerAdmissionTest(unittest.TestCase):
    def setUp(self):
        self.pin = {'source_sha': 'a' * 40, 'run_id': 10, 'artifact_id': 20,
                    'artifact_sha256': 'b' * 64, 'package_sha256': 'c' * 64}
        source = self.pin['source_sha']
        self.name = 'native-desktop-viewer-linux-amd64-' + source
        self.run = {'id': 10, 'head_sha': source, 'head_branch': 'dev', 'event': 'push',
                    'status': 'completed', 'conclusion': 'success', 'path': WORKFLOW,
                    'repository': {'full_name': REPOSITORY}}
        self.artifact = {'id': 20, 'name': self.name, 'expired': False, 'size_in_bytes': 100,
                         'digest': 'sha256:' + self.pin['artifact_sha256'],
                         'workflow_run': {'id': 10, 'head_sha': source}}

    def api(self):
        provider = Mock()
        provider.get.side_effect = [self.run, self.artifact]
        return provider

    def test_requires_an_exact_successful_dev_artifact(self):
        self.assertEqual(admit_viewer(self.pin, self.api()), self.name)
        self.artifact['expired'] = True
        with self.assertRaisesRegex(PublicationStopped, 'workflow binding'):
            admit_viewer(self.pin, self.api())
        self.run['event'] = 'pull_request'
        with self.assertRaisesRegex(PublicationStopped, 'owning dev workflow'):
            admit_viewer(self.pin, self.api())

    def test_distribution_review_must_bind_the_artifact_and_pass_every_check(self):
        with tempfile.TemporaryDirectory() as directory:
            source_root = Path(directory)
            relative = 'release/catalog-qualification/viewer.json'
            path = source_root / relative
            path.parent.mkdir(parents=True)
            review = {'schema': 1, 'source_sha': self.pin['source_sha'],
                      'artifact_sha256': self.pin['artifact_sha256'],
                      'package_sha256': self.pin['package_sha256'], 'platform': 'linux-amd64',
                      'package': 'resolver/dockpipe.desktop.native', 'checks': {
                          'media-source-and-build': 'passed', 'license-and-relinking': 'passed',
                          'codec-distribution': 'passed', 'installed-viewer': 'passed',
                          'paired-media-input-cleanup': 'passed'}}

            def update_review():
                payload = json.dumps(review).encode()
                path.write_bytes(payload)
                self.pin['distribution_review'] = {
                    'path': relative, 'sha256': hashlib.sha256(payload).hexdigest(),
                    'size_bytes': len(payload)}

            update_review()
            admit_distribution_review(self.pin, source_root)
            review['checks']['codec-distribution'] = 'pending'
            update_review()
            with self.assertRaisesRegex(PublicationStopped, 'have not been qualified'):
                admit_distribution_review(self.pin, source_root)
