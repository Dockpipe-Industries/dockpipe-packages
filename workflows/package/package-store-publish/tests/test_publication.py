import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

MODULE = Path(__file__).parents[1] / "assets/scripts/publication.py"
SPEC = importlib.util.spec_from_file_location("store_publication", MODULE)
PUBLICATION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PUBLICATION)


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.artifacts = self.root / "release/artifacts"
        self.artifacts.mkdir(parents=True)
        self.output = self.root / "output"
        self.environment = {
            "DOCKPIPE_SOURCE_ROOT": str(self.root),
            "R2_BUCKET": "example-bucket",
            "R2_ENDPOINT_URL": "https://example.invalid",
            "R2_PREFIX": "/packages/",
        }

    def files(self):
        for name in ("package-1.0.tar.gz", "package-1.0.tar.gz.sha256",
                     "packages-store-manifest.json", "packages-store-manifest.json.sha256"):
            (self.artifacts / name).write_text(name)

    def verified_state(self):
        manifest = json.loads((self.output / "manifest.json").read_text())
        objects = [dict(item, publish_last=False) for item in manifest["objects"]]
        identity = hashlib.sha256(json.dumps({
            "endpoint": "https://example.invalid", "bucket": "example-bucket",
            "objects": objects, "trusted": {},
        }, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        state = {"schema": 1, "phase": "verified-head", "identity": identity,
                 "endpoint": "https://example.invalid", "bucket": "example-bucket"}
        (self.output / "state.json").write_text(json.dumps(state))

    def test_manifest_preserves_keys_content_types_and_separates_metadata(self):
        self.files()
        environment = PUBLICATION.prepare(self.environment, self.output)
        manifest = json.loads(Path(environment["R2_BULK_MANIFEST"]).read_text())
        self.assertEqual([item["key"] for item in manifest["objects"]], [
            "packages/package-1.0.tar.gz", "packages/package-1.0.tar.gz.sha256",
        ])
        self.assertEqual(manifest["objects"][0]["content_type"], "application/gzip")
        self.assertEqual(environment["R2_BULK_DRY_RUN"], "true")
        self.assertEqual(len(json.loads((self.output / "metadata.json").read_text())["metadata"]), 2)

    def test_skip_does_not_require_files_or_destination(self):
        environment = PUBLICATION.prepare({"PACKAGE_RELEASE_SKIP_UPLOAD": "1"}, self.output)
        self.assertEqual(environment["R2_BULK_SKIP"], "true")
        with patch.object(PUBLICATION.subprocess, "run") as provider:
            PUBLICATION.publish_metadata(self.output / "metadata.json", Path("dockpipe"))
        provider.assert_not_called()

    def test_dry_run_never_writes_metadata_or_requires_verified_state(self):
        self.files()
        PUBLICATION.prepare(self.environment, self.output)
        with patch.object(PUBLICATION.subprocess, "run") as provider:
            PUBLICATION.publish_metadata(self.output / "metadata.json", Path("dockpipe"))
        provider.assert_not_called()

    def test_missing_or_wrong_payload_proof_blocks_metadata(self):
        self.files()
        self.environment["R2_PUBLISH_DRY_RUN"] = "0"
        PUBLICATION.prepare(self.environment, self.output)
        (self.output / "state.json").write_text(json.dumps({"schema": 1, "phase": "transferring"}))
        with patch.object(PUBLICATION.subprocess, "run") as provider:
            with self.assertRaisesRegex(ValueError, "verification"):
                PUBLICATION.publish_metadata(self.output / "metadata.json", Path("dockpipe"))
        provider.assert_not_called()

    def test_metadata_publishes_in_original_order_only_after_exact_payload_proof(self):
        self.files()
        self.environment["R2_PUBLISH_DRY_RUN"] = "0"
        PUBLICATION.prepare(self.environment, self.output)
        self.verified_state()
        with patch.object(PUBLICATION.subprocess, "run") as provider:
            PUBLICATION.publish_metadata(self.output / "metadata.json", Path("dockpipe"))
        self.assertEqual([call.args[0][3] for call in provider.call_args_list], [
            str(self.artifacts / "packages-store-manifest.json"),
            str(self.artifacts / "packages-store-manifest.json.sha256"),
        ])

    def test_outer_flags_translate_to_strict_bulk_boolean_contract(self):
        self.files()
        for value, expected in (("1", "true"), ("true", "true"), ("0", "false")):
            with self.subTest(value=value):
                environment = dict(self.environment, PACKAGE_RELEASE_SKIP_UPLOAD=value,
                                   R2_PUBLISH_DRY_RUN=value)
                prepared = PUBLICATION.prepare(environment, self.output)
                self.assertEqual(prepared["R2_BULK_SKIP"], expected)
                self.assertEqual(prepared["R2_BULK_DRY_RUN"], expected)

    def test_changed_payload_manifest_identity_blocks_all_metadata_writes(self):
        self.files()
        self.environment["R2_PUBLISH_DRY_RUN"] = "0"
        PUBLICATION.prepare(self.environment, self.output)
        self.verified_state()
        manifest_path = self.output / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["objects"][0]["content_type"] = "application/octet-stream"
        manifest_path.write_text(json.dumps(manifest))
        with patch.object(PUBLICATION.subprocess, "run") as provider:
            with self.assertRaisesRegex(ValueError, "verification"):
                PUBLICATION.publish_metadata(self.output / "metadata.json", Path("dockpipe"))
        provider.assert_not_called()

    def test_changed_metadata_blocks_all_metadata_writes(self):
        self.files()
        self.environment["R2_PUBLISH_DRY_RUN"] = "0"
        PUBLICATION.prepare(self.environment, self.output)
        self.verified_state()
        (self.artifacts / "packages-store-manifest.json.sha256").write_text("changed")
        with patch.object(PUBLICATION.subprocess, "run") as provider:
            with self.assertRaisesRegex(ValueError, "changed"):
                PUBLICATION.publish_metadata(self.output / "metadata.json", Path("dockpipe"))
        provider.assert_not_called()


if __name__ == "__main__":
    unittest.main()
