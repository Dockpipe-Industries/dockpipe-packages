"""Failure artifacts remain separate from successful immutable publication."""
from pathlib import Path
import unittest

import yaml


class FailureEvidenceTests(unittest.TestCase):
    def test_native_failure_upload_does_not_bypass_publication_dependency(self):
        source = Path(__file__).resolve().parents[4]
        workflow = yaml.safe_load((source / '.github/workflows/native-store-candidate.yml').read_text())
        native = workflow['jobs']['native']
        upload = next(step for step in native['steps'] if step.get('uses') == 'actions/upload-artifact@v4')
        self.assertEqual(upload['if'], 'always()')
        self.assertEqual(upload['with']['if-no-files-found'], 'error')
        self.assertIn('native', workflow['jobs']['publish']['needs'])
        self.assertNotIn('always()', workflow['jobs']['publish']['if'])


if __name__ == '__main__':
    unittest.main()
