"""Flatpak candidates cannot silently reuse or relabel the native SDK."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest import mock


PACKAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE / "tooling"))
SPEC = importlib.util.spec_from_file_location("flatpak_producer", PACKAGE / "tooling/build_flatpak.py")
PRODUCER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PRODUCER)


class FlatpakProducerTest(unittest.TestCase):
    def test_generated_viewer_profile_excludes_incoming_worker_declarations(self):
        source = ("# native source stays intact\n"
                  "DOCKPIPE_REMOTE_DESKTOP_DISPLAY=assets/display/display-factory.json\n"
                  "DOCKPIPE_REMOTE_DESKTOP_CHANNEL=assets/tooling/bin/channel\n"
                  "DOCKPIPE_REMOTE_DESKTOP_WORKER=assets/tooling/worker.app/worker\n"
                  "DOCKPIPE_REMOTE_DESKTOP_NATIVE_WORKER=assets/worker/worker-factory.json\n")
        viewer = PRODUCER.viewer_profile(source)
        self.assertIn("DOCKPIPE_REMOTE_DESKTOP_DISPLAY=", viewer)
        self.assertIn("DOCKPIPE_REMOTE_DESKTOP_CHANNEL=", viewer)
        self.assertNotIn("DOCKPIPE_REMOTE_DESKTOP_WORKER=", viewer)
        self.assertNotIn("DOCKPIPE_REMOTE_DESKTOP_NATIVE_WORKER=", viewer)
        self.assertIn("DOCKPIPE_REMOTE_DESKTOP_WORKER=", source)
        with self.assertRaisesRegex(ValueError, "unique expected worker"):
            PRODUCER.viewer_profile(viewer)

    def prepare_fixture(self, root, compiler="15.2.0"):
        prefix = root / "DockPipeDisplaySDK-1.0.0-linux-amd64"
        header = b"pinned display interface\n"
        scope = b"pinned native library scope\n"
        profile = dict(PRODUCER.PROFILE, interface_sha256=hashlib.sha256(header).hexdigest(),
                       native_library_scope_sha256=hashlib.sha256(scope).hexdigest())
        candidate = {key: profile[key] for key in
                     ("interface", "qt_version", "interface_sha256", "native_library_scope_sha256")}
        for relative in PRODUCER.SDK_PATHS:
            path = prefix / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"SDK fixture\n")
        (prefix / "include/dockpipe-launcher/LauncherDisplayPackage.h").write_bytes(header)
        (prefix / "include/dockpipe-launcher/LauncherNativeLibraryScope.h").write_bytes(scope)
        (prefix / "share/DockPipeDisplaySDK/display-sdk-candidate.json").write_text(json.dumps(candidate))
        entries = [{"path": relative, "bytes": (prefix / relative).stat().st_size,
                    "sha256": PRODUCER.dependencies.digest(prefix / relative)}
                   for relative in sorted(PRODUCER.SDK_PATHS)]
        manifest = root / "sdk-files.json"
        manifest.write_text(json.dumps({"schema": 1, "files": entries}))
        archive = root / (prefix.name + ".tar.gz")
        with tarfile.open(archive, "w:gz") as output:
            for row in entries:
                output.add(prefix / row["path"], arcname=prefix.name + "/" + row["path"])
        release = {"schema": 1, "product": "DockPipeDisplaySDK", "version": "1.0.0",
                   "platform": "linux-amd64", "qt_version": profile["qt_version"],
                   "interface": profile["interface"], "compiler": {"id": "GNU", "version": compiler},
                   "source_state": "development",
                   "checks": ["sdk-component-install", "relocated-external-consumer-compile-link", "native-consumer-run"],
                   "archive": {"name": archive.name, "size_bytes": archive.stat().st_size,
                               "sha256": PRODUCER.dependencies.digest(archive)},
                   "file_manifest": {"name": manifest.name, "sha256": PRODUCER.dependencies.digest(manifest)}}
        release_path = root / "sdk-release.json"
        release_path.write_text(json.dumps(release))
        return profile, PRODUCER.dependencies.digest(release_path)

    def test_separate_sdk_verifies_and_relocates_without_native_config_rewrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            profile, digest = self.prepare_fixture(root)
            with mock.patch.object(PRODUCER, "PROFILE", profile):
                release = PRODUCER.prepare_display_sdk(root, digest, root / "relocated", True)
            self.assertEqual(release["compiler"]["version"], "15.2.0")
            self.assertEqual((root / "relocated/lib/cmake/DockPipeDisplaySDK/DockPipeDisplaySDKConfig.cmake").read_bytes(),
                             b"SDK fixture\n")

    def test_native_compiler_artifact_cannot_be_relabelled_as_flatpak(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            profile, digest = self.prepare_fixture(root, compiler="11.4.0")
            with mock.patch.object(PRODUCER, "PROFILE", profile):
                with self.assertRaisesRegex(ValueError, "separate Flatpak ABI"):
                    PRODUCER.prepare_display_sdk(root, digest, root / "relocated", True)

    def test_development_sdk_requires_explicit_opt_in(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            profile, digest = self.prepare_fixture(root)
            with mock.patch.object(PRODUCER, "PROFILE", profile):
                with self.assertRaisesRegex(ValueError, "allow-development-sdk"):
                    PRODUCER.prepare_display_sdk(root, digest, root / "relocated", False)

    def test_changed_release_or_archive_cannot_enter_candidate(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            profile, digest = self.prepare_fixture(root)
            with mock.patch.object(PRODUCER, "PROFILE", profile):
                with self.assertRaisesRegex(ValueError, "release digest"):
                    PRODUCER.prepare_display_sdk(root, "0" * 64, root / "relocated", True)
                archive = root / "DockPipeDisplaySDK-1.0.0-linux-amd64.tar.gz"
                archive.write_bytes(archive.read_bytes() + b"changed")
                with self.assertRaisesRegex(ValueError, "size or SHA-256"):
                    PRODUCER.prepare_display_sdk(root, digest, root / "relocated", True)


if __name__ == "__main__":
    unittest.main()
