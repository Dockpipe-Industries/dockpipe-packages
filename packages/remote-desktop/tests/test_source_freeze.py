"""Source admission rejects a partial or changed component-owner freeze."""
from pathlib import Path
import hashlib
import importlib.util
import json
import sys
import tempfile
import unittest
from unittest.mock import patch


PACKAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE / "tooling"))
SPEC = importlib.util.spec_from_file_location("desktop_build", PACKAGE / "tooling/build.py")
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)


class SourceFreezeTest(unittest.TestCase):
    def test_partial_owner_freeze_is_not_a_buildable_candidate(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "dependencies").mkdir()
            (root / "dependencies/source-files.lock.json").write_text(
                json.dumps({"status": "partial-owner-freeze", "files": []}))
            with patch.object(BUILD, "PACKAGE", root), self.assertRaisesRegex(ValueError, "freeze is incomplete"):
                BUILD.verify_source()

    def test_owner_freeze_rejects_mutated_and_unlisted_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "dependencies").mkdir()
            (root / "channel").mkdir()
            path = root / "channel/main.go"
            path.write_bytes(b"reviewed")
            lock = {"status": "owner-frozen", "files": [{"path": "channel/main.go", "bytes": 8,
                    "sha256": hashlib.sha256(b"reviewed").hexdigest()}]}
            (root / "dependencies/source-files.lock.json").write_text(json.dumps(lock))
            with patch.object(BUILD, "PACKAGE", root):
                self.assertEqual(BUILD.verify_source(), lock)
                path.write_bytes(b"modified")
                with self.assertRaisesRegex(ValueError, "mismatch"):
                    BUILD.verify_source()
                path.write_bytes(b"reviewed")
                (root / "channel/unreviewed.go").write_bytes(b"unreviewed")
                with self.assertRaisesRegex(ValueError, "differ from the owner-frozen inventory"):
                    BUILD.verify_source()


if __name__ == "__main__":
    unittest.main()
