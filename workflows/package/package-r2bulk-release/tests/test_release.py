import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "r2bulk_release", Path(__file__).resolve().parents[1] / "assets/scripts/release.py")
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def test_catalog_refuses_unrelated_or_wrong_version_packages(self):
        workflow = {"name": release.NAME, "version": "0.1.1", "tarball": "package.tar.gz"}
        self.assertEqual(release.package_entry({"packages": {"workflows": [workflow]}}), workflow)
        cases = [
            {"workflows": [workflow, workflow]},
            {"workflows": [workflow], "resolvers": [workflow]},
            {"workflows": [dict(workflow, version="0.1.0")]},
            {"workflows": [dict(workflow, tarball="../package.tar.gz")]},
        ]
        for packages in cases:
            with self.subTest(packages=packages), self.assertRaises(ValueError):
                release.package_entry({"packages": packages})

    def test_archive_rejects_traversal_and_symlinks(self):
        for name, member_type in (("../escape", tarfile.REGTYPE), ("link", tarfile.SYMTYPE)):
            archive = self.root / "bad.tar.gz"
            with tarfile.open(archive, "w:gz") as bundle:
                member = tarfile.TarInfo(name)
                member.type = member_type
                bundle.addfile(member)
            with self.subTest(name=name), self.assertRaises(ValueError):
                release.extract(archive, self.root / "unpacked")
        self.assertFalse((self.root / "escape").exists())

    def qualified_assets(self):
        assets = self.root / "assets"
        assets.mkdir()
        archive = assets / "package.tar.gz"
        archive.write_bytes(b"qualified fixture")
        archive_hash = hashlib.sha256(archive.read_bytes()).hexdigest()
        for name in ("qualification.json", "provenance.json"):
            (assets / name).write_text(json.dumps({"source_commit": "a" * 40, "archive_sha256": archive_hash}))
        manifest = {"packages": {"workflows": [{"name": release.NAME, "version": release.VERSION,
                    "tarball": archive.name, "sha256": archive_hash}]}}
        (assets / "packages-store-manifest.json").write_text(json.dumps(manifest))
        self.refresh_checksums(assets)
        return assets

    def refresh_checksums(self, assets):
        paths = sorted(path for path in assets.iterdir() if path.name != "SHA256SUMS")
        (assets / "SHA256SUMS").write_text("".join(
            hashlib.sha256(path.read_bytes()).hexdigest() + "  " + path.name + "\n" for path in paths))

    def test_default_publish_is_provider_free_and_explicit_flag_is_strict(self):
        self.qualified_assets()
        with patch.dict(release.os.environ, {}, clear=True), patch.object(release.subprocess, "run") as command:
            with patch("sys.stdout", new_callable=io.StringIO) as stdout:
                release.publish(self.root, "a" * 40)
            self.assertTrue(json.loads(stdout.getvalue())["dry_run"])
            command.assert_not_called()
        with patch.dict(release.os.environ, {"R2BULK_RELEASE_EXECUTE": "yes"}), self.assertRaises(ValueError):
            release.publish(self.root, "a" * 40)

    def test_changed_missing_extra_or_wrong_source_asset_blocks_publish(self):
        assets = self.qualified_assets()
        with self.assertRaises(ValueError):
            release.verify_assets(assets, "b" * 40)
        archive = assets / "package.tar.gz"
        archive.write_bytes(b"modified")
        with self.assertRaises(ValueError):
            release.verify_assets(assets, "a" * 40)
        archive.write_bytes(b"qualified fixture")
        self.refresh_checksums(assets)
        (assets / "extra").write_bytes(b"unqualified")
        with self.assertRaises(ValueError):
            release.verify_assets(assets, "a" * 40)
        (assets / "extra").unlink()
        archive.unlink()
        with self.assertRaises(FileNotFoundError):
            release.verify_assets(assets, "a" * 40)

    def test_only_http_404_proves_remote_release_absence(self):
        responses = [
            (0, "HTTP/2.0 200 OK\n", True),
            (1, "HTTP/2.0 401 Unauthorized\n", True),
            (1, "", True),
            (1, "HTTP/2.0 404 Not Found\n", False),
        ]
        for code, stdout, rejected in responses:
            result = subprocess.CompletedProcess([], code, stdout, "fixture")
            with self.subTest(stdout=stdout), patch.object(release.subprocess, "run", return_value=result):
                if rejected:
                    with self.assertRaises(ValueError):
                        release.require_absent("fixture")
                else:
                    release.require_absent("fixture")

    def test_remote_tag_must_resolve_to_exact_qualified_commit(self):
        commit = {"object": {"type": "commit", "sha": "a" * 40}}
        annotated = {"object": {"type": "tag", "sha": "b" * 40}}
        with patch.object(release, "output", side_effect=[json.dumps(annotated), json.dumps(commit)]):
            release.verify_tag("a" * 40)
        with patch.object(release, "output", return_value=json.dumps(commit)), self.assertRaises(ValueError):
            release.verify_tag("c" * 40)


if __name__ == "__main__":
    unittest.main()
