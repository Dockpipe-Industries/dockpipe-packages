import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

MODULE_PATH = Path(__file__).resolve().parents[1] / 'tooling/materialize_bundle.py'
SPECIFICATION = importlib.util.spec_from_file_location('materialize_bundle', MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPECIFICATION)
SPECIFICATION.loader.exec_module(MODULE)


class BundleMaterializationTests(unittest.TestCase):
    def make_artifact(self, root, extra=None):
        files = {'dockpipe-guest-tools.exe': b'fixture executable',
                 'install-guest-tools.ps1': b'fixture installer',
                 'licenses/THIRD-PARTY.txt': b'fixture licenses'}
        inventory = {'schema': 1, 'product': 'DockpipeGuestTools', 'version': '1.0.0',
                     'platform': 'windows-amd64', 'qt_version': '6.10.3',
                     'files': [{'path': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                               for name, data in files.items()]}
        files['inventory.json'] = json.dumps(inventory).encode()
        if extra:
            files.update(extra)
        archive = root / 'guest-tools.zip'
        with zipfile.ZipFile(archive, 'w') as output:
            for name, data in files.items():
                output.writestr(name, data)
        descriptor = {key: value for key, value in inventory.items() if key != 'files'}
        descriptor.update(entrypoint='dockpipe-guest-tools.exe', installer='install-guest-tools.ps1',
                          media_subdirectory='GuestTools', inventory_sha256=hashlib.sha256(files['inventory.json']).hexdigest(),
                          archive={'name': archive.name, 'size_bytes': archive.stat().st_size,
                                   'sha256': hashlib.sha256(archive.read_bytes()).hexdigest()})
        metadata = root / 'release.json'
        metadata.write_text(json.dumps(descriptor))
        return archive, metadata

    def test_exact_bundle_publishes_without_source_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, metadata = self.make_artifact(root)
            destination = root / 'installed'
            MODULE.materialize(archive, metadata, destination)
            self.assertEqual((destination / 'dockpipe-guest-tools.exe').read_bytes(), b'fixture executable')
            with self.assertRaises(ValueError):
                MODULE.materialize(archive, metadata, destination)

    def test_archive_identity_mismatch_does_not_publish(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, metadata = self.make_artifact(root)
            with archive.open('ab') as output:
                output.write(b'tampered')
            with self.assertRaises(ValueError):
                MODULE.materialize(archive, metadata, root / 'installed')
            self.assertFalse((root / 'installed').exists())

    def test_traversal_unlisted_and_case_duplicate_entries_are_rejected(self):
        for name in ('../escape', 'unlisted.dll', 'DOCKPIPE-GUEST-TOOLS.EXE'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                archive, metadata = self.make_artifact(root, {name: b'unsafe'})
                with self.assertRaises(ValueError):
                    MODULE.materialize(archive, metadata, root / 'installed')
                self.assertFalse((root / 'installed').exists())
                self.assertFalse((root.parent / 'escape').exists())


if __name__ == '__main__':
    unittest.main()
