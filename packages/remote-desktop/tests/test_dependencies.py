"""Fixture-only checks for native dependency integrity and extraction bounds."""
from pathlib import Path
import hashlib
import importlib.util
import io
import json
import tarfile
import tempfile
import unittest


PACKAGE = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("desktop_dependencies", PACKAGE / "tooling/dependencies.py")
DEPENDENCIES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DEPENDENCIES)


def fixture_archive(path, entries):
    with tarfile.open(path, "w:gz") as archive:
        for name, content, kind in entries:
            member = tarfile.TarInfo(name)
            member.type = kind
            if kind == tarfile.REGTYPE:
                member.size = len(content)
                archive.addfile(member, io.BytesIO(content))
            else:
                member.linkname = content.decode()
                archive.addfile(member)


class DependencyAdmissionTest(unittest.TestCase):
    def test_changed_file_rejected_before_use(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "input"
            path.write_bytes(b"original")
            record = {"bytes": 8, "sha256": hashlib.sha256(b"original").hexdigest()}
            DEPENDENCIES.verify_file(path, record)
            path.write_bytes(b"modified")
            with self.assertRaisesRegex(ValueError, "mismatch"):
                DEPENDENCIES.verify_file(path, record)

    def test_symlink_input_is_not_accepted_as_an_artifact(self):
        with tempfile.TemporaryDirectory() as temporary:
            original = Path(temporary) / "original"
            original.write_bytes(b"fixture")
            alias = Path(temporary) / "alias"
            alias.symlink_to(original)
            record = {"bytes": 7, "sha256": hashlib.sha256(b"fixture").hexdigest()}
            with self.assertRaisesRegex(ValueError, "regular file"):
                DEPENDENCIES.verify_file(alias, record)

    def test_archive_rejects_duplicate_escape_and_alias(self):
        cases = [
            [("root/file", b"first", tarfile.REGTYPE), ("root/file", b"second", tarfile.REGTYPE)],
            [("root/../../escaped", b"data", tarfile.REGTYPE)],
            [("root/alias", b"../../outside", tarfile.SYMTYPE)],
        ]
        with tempfile.TemporaryDirectory() as temporary:
            for index, entries in enumerate(cases):
                with self.subTest(case=index):
                    path = Path(temporary) / (str(index) + ".tar.gz")
                    fixture_archive(path, entries)
                    with tarfile.open(path) as archive, self.assertRaises(ValueError):
                        DEPENDENCIES.archive_files(archive)

    def test_selected_extraction_enforces_aggregate_bound_before_writes(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            archive_path = directory / "input.tar.gz"
            fixture_archive(archive_path, [("root/file", b"small", tarfile.REGTYPE)])
            with tarfile.open(archive_path) as archive:
                files = DEPENDENCIES.archive_files(archive)
                member = files["root/file"]
                member.size = DEPENDENCIES.MAX_SELECTED_BYTES + 1
                with self.assertRaisesRegex(ValueError, "extraction bound"):
                    DEPENDENCIES.extract_selected(archive, [("file", member)], directory / "output")
            self.assertFalse((directory / "output").exists())

    def test_sdk_must_match_release_manifest_and_payload(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            sdk = directory / "linux-amd64"
            sdk.mkdir()
            entries = [{"path": "include/dockpipe-launcher/LauncherDisplayPackage.h", "bytes": 6,
                        "sha256": hashlib.sha256(b"header").hexdigest()}]
            entries.extend({"path": "payload/" + str(i), "bytes": 1,
                            "sha256": hashlib.sha256(b"x").hexdigest()} for i in range(6))
            archive_name = "sdk.tar.gz"
            archive_entries = [("DockPipeDisplaySDK-1.0.0-linux-amd64/" + entry["path"],
                                b"header" if i == 0 else b"x", tarfile.REGTYPE)
                               for i, entry in enumerate(entries)]
            fixture_archive(sdk / archive_name, archive_entries)
            release = {"product": "DockPipeDisplaySDK", "version": "1.0.0", "platform": "linux-amd64",
                       "interface": "com.dockpipe.launcher.DisplayPackage/1.0", "qt_version": "6.10.3",
                       "source_sha": "a" * 40, "archive": {"name": archive_name}}
            (sdk / "sdk-release.json").write_text(json.dumps(release))
            (sdk / "sdk-files.json").write_text(json.dumps({"schema": 1, "files": entries}))
            records = [{"platform": "linux-amd64", "name": name, "bytes": (sdk / name).stat().st_size,
                        "sha256": DEPENDENCIES.digest(sdk / name)}
                       for name in [archive_name, "sdk-release.json", "sdk-files.json"]]
            lock = {"host_sdk": {"artifacts": records, "interface": release["interface"],
                                 "source_commit": release["source_sha"], "header_sha256": entries[0]["sha256"]},
                    "qt": {"version": "6.10.3"}}
            DEPENDENCIES.prepare_sdk(directory, directory / "verified", "linux-amd64", lock)
            self.assertEqual((directory / "verified/sdk/include/dockpipe-launcher/LauncherDisplayPackage.h").read_bytes(), b"header")
            # An extra payload is forbidden even when the archive's outer hash is newly reviewed.
            archive_entries.append(("DockPipeDisplaySDK-1.0.0-linux-amd64/unlisted", b"extra", tarfile.REGTYPE))
            fixture_archive(sdk / archive_name, archive_entries)
            records[0].update(bytes=(sdk / archive_name).stat().st_size,
                              sha256=DEPENDENCIES.digest(sdk / archive_name))
            with self.assertRaisesRegex(ValueError, "differs from its file inventory"):
                DEPENDENCIES.prepare_sdk(directory, directory / "rejected", "linux-amd64", lock)

    def test_host_library_alias_is_contained_and_its_bytes_remain_pinned(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix = root / "host"
            prefix.mkdir()
            library = prefix / "libfixture.so.1"
            library.write_bytes(b"reviewed")
            alias = prefix / "libfixture.so"
            alias.symlink_to(library.name)
            record = {"file": alias.name, "resolved_file": library.name, "bytes": 8,
                      "sha256": hashlib.sha256(b"reviewed").hexdigest()}
            lock = {"linux_host_runtime": {"libraries": [record]}}
            self.assertEqual(len(DEPENDENCIES.verify_host_runtime(prefix, lock)), 1)
            library.write_bytes(b"modified")
            with self.assertRaisesRegex(ValueError, "mismatch"):
                DEPENDENCIES.verify_host_runtime(prefix, lock)
            outside = root / library.name
            outside.write_bytes(b"reviewed")
            alias.unlink()
            alias.symlink_to(outside)
            with self.assertRaisesRegex(ValueError, "escapes"):
                DEPENDENCIES.verify_host_runtime(prefix, lock)

    def test_worker_sdk_header_is_an_independent_pinned_artifact(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            header = root / "DockpipeNativeWorkerPackage.h"
            header.write_bytes(b"reviewed worker ABI")
            record = {"schema": "dockpipe.native-worker-sdk/v1", "abi_version": 1,
                      "entrypoint": "dockpipe_native_worker_package_v1", "path": header.name,
                      "bytes": header.stat().st_size, "sha256": DEPENDENCIES.digest(header)}
            lock = {"native_worker_sdk": record}
            DEPENDENCIES.prepare_worker_sdk(root, root / "accepted", lock)
            self.assertEqual((root / "accepted/worker-sdk/include" / header.name).read_bytes(), header.read_bytes())
            header.write_bytes(b"changed worker ABI!")
            with self.assertRaisesRegex(ValueError, "mismatch"):
                DEPENDENCIES.prepare_worker_sdk(root, root / "rejected", lock)
            self.assertFalse((root / "rejected").exists())
            record["abi_version"] = 2
            with self.assertRaisesRegex(ValueError, "Unsupported"):
                DEPENDENCIES.prepare_worker_sdk(root, root / "wrong-abi", lock)
            self.assertFalse((root / "wrong-abi").exists())

    def test_media_sources_are_copied_only_when_every_pin_matches(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            ffmpeg = root / "ffmpeg.tar.gz"
            qt = root / "qtmultimedia.tar.xz"
            ffmpeg.write_bytes(b"frozen FFmpeg source")
            qt.write_bytes(b"frozen Qt source")
            records = [{"file": path.name, "bytes": path.stat().st_size,
                        "sha256": DEPENDENCIES.digest(path)} for path in (ffmpeg, qt)]
            distribution = {"ffmpeg_version": "7.1.3", "qt_multimedia_version": "6.10.3",
                            "sources": records}
            lock = {"ffmpeg": {"linux-amd64_distribution": distribution}}
            accepted = root / "accepted"
            DEPENDENCIES.copy_linux_media_sources(root, accepted, lock)
            self.assertEqual((accepted / ffmpeg.name).read_bytes(), ffmpeg.read_bytes())
            manifest = json.loads((accepted / "manifest.json").read_text())
            self.assertEqual(manifest["sources"], records)
            self.assertEqual(manifest["ffmpeg_version"], "7.1.3")

            qt.write_bytes(b"changed Qt source")
            rejected = root / "rejected"
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                DEPENDENCIES.copy_linux_media_sources(root, rejected, lock)
            self.assertFalse(rejected.exists())

    def test_unsupported_platform_fails_before_creating_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaisesRegex(ValueError, "Only Linux"):
                DEPENDENCIES.prepare(root, root / "output", "windows-amd64")
            self.assertFalse((root / "output").exists())


if __name__ == "__main__":
    unittest.main()
