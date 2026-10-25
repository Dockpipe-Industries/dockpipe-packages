"""Check the package backend boundary and preserved Windows service sources."""
import hashlib
import json
from pathlib import Path
import unittest

PACKAGE = Path(__file__).resolve().parents[1]


class BackendAssetTests(unittest.TestCase):
    def test_migrated_backend_matches_recorded_postimages(self):
        document = json.loads((PACKAGE / "guest-services/backend-origin.json").read_text())
        self.assertEqual(document["schema"], 1)
        for item in document["files"]:
            relative = Path(item["destination"]).relative_to("packages/vm")
            with self.subTest(path=str(relative)):
                digest = hashlib.sha256((PACKAGE / relative).read_bytes()).hexdigest()
                self.assertEqual(digest, item["destination_sha256"])
                if relative.name != "vmimage-run.sh":
                    self.assertEqual(digest, item["source_sha256"])

    def test_resolver_selects_its_own_adapter(self):
        profile = (PACKAGE / "resolvers/qemu/profile").read_text()
        self.assertIn("DOCKPIPE_RESOLVER_HOST_ISOLATE=scripts/qemu/vmimage-run.sh", profile)
        self.assertIn("DOCKPIPE_RESOLVER_HOST_REQUIRES_DOCKER=0", profile)
        runner = (PACKAGE / "resolvers/qemu/assets/scripts/vmimage-run.sh").read_text()
        self.assertNotIn('source "${SCRIPT_DIR}/lib/dockpipe-sdk.sh"', runner)
        self.assertIn('"$sdk_binary" sdk --workdir', runner)


if __name__ == "__main__":
    unittest.main()
