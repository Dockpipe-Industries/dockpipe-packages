"""Verify locked installation and rejection before replacing an installed CLI."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

SPEC = importlib.util.spec_from_file_location(
    'bootstrap_dockpipe', Path(__file__).with_name('bootstrap_dockpipe.py'))
bootstrap = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bootstrap)


class LockedInstallationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.output = self.root / 'installed'
        self.output.mkdir()
        self.archive = self.root / 'archive'
        self.environment = self.root / 'github-env'
        self.config = {'dockpipe_version': '0.6.25'}
        self.entry = {
            'url': 'https://github.com/Dockpipe-Industries/dockpipe/releases/download/v0.6.25/cli.tar.gz',
            'executable': 'dockpipe',
            'executable_size_bytes': 3,
        }
        self.lock = {'schema': 1, 'dockpipe': {
            'version': '0.6.25', 'artifacts': {'linux/amd64': self.entry}}}
        self.addCleanup(patch.stopall)
        patch.object(bootstrap, 'ROOT', self.root).start()
        patch.object(bootstrap.platform, 'system', return_value='Linux').start()
        patch.object(bootstrap.platform, 'machine', return_value='x86_64').start()
        self.version = patch.object(bootstrap.subprocess, 'check_output', return_value='0.6.25\n').start()
        patch.dict(bootstrap.os.environ, {'GITHUB_ENV': str(self.environment)}).start()
        self.write_archive()
        self.write_configuration()
        (self.output / 'dockpipe').write_bytes(b'previous')

    def write_archive(self, windows=False, link=False):
        if windows:
            with zipfile.ZipFile(self.archive, 'w') as bundle:
                bundle.writestr('dockpipe.exe', b'cli')
        else:
            with tarfile.open(self.archive, 'w:gz') as bundle:
                member = tarfile.TarInfo('dockpipe')
                member.size = 3
                if link:
                    member.type = tarfile.SYMTYPE
                    member.linkname = '/outside'
                bundle.addfile(member, io.BytesIO(b'cli'))
        data = self.archive.read_bytes()
        self.entry.update(size_bytes=len(data), sha256=hashlib.sha256(data).hexdigest())

    def write_configuration(self):
        (self.root / 'dockpipe.config.json').write_text(json.dumps(self.config))
        (self.root / 'dockpipe.lock.json').write_text(json.dumps(self.lock))

    def assert_rejected(self, message):
        self.write_configuration()
        with self.assertRaisesRegex(ValueError, message):
            bootstrap.install(self.output, self.archive)
        self.assertEqual((self.output / 'dockpipe').read_bytes(), b'previous')
        self.assertFalse(self.environment.exists())

    def test_linux_install_and_environment(self):
        installed = bootstrap.install(self.output, self.archive)
        self.assertEqual(installed.read_bytes(), b'cli')
        self.assertEqual(self.environment.read_text(), 'DOCKPIPE_BIN=' + str(installed) + '\n')

    def test_windows_archive_and_amd64_alias(self):
        patch.object(bootstrap.platform, 'system', return_value='Windows').start()
        patch.object(bootstrap.platform, 'machine', return_value='AMD64').start()
        self.entry['executable'] = 'dockpipe.exe'
        self.lock['dockpipe']['artifacts'] = {'windows/amd64': self.entry}
        self.write_archive(windows=True)
        self.write_configuration()
        self.assertEqual(bootstrap.install(self.output, self.archive).name, 'dockpipe.exe')

    def test_version_disagreement(self):
        self.config['dockpipe_version'] = '1.0.0'
        self.assert_rejected('config and lock')

    def test_checksum_disagreement(self):
        self.entry['sha256'] = '0' * 64
        self.assert_rejected('checksum mismatch')
        self.version.assert_not_called()

    def test_archive_size_disagreement(self):
        self.entry['size_bytes'] += 1
        self.assert_rejected('archive size mismatch')

    def test_executable_size_disagreement(self):
        self.entry['executable_size_bytes'] += 1
        self.assert_rejected('regular file of the expected size')

    def test_symbolic_link_rejected(self):
        self.write_archive(link=True)
        self.assert_rejected('regular file')

    def test_embedded_version_disagreement(self):
        self.version.return_value = '0.6.24\n'
        self.assert_rejected('version differs')

    def test_unsupported_architecture(self):
        patch.object(bootstrap.platform, 'machine', return_value='unknown').start()
        self.assert_rejected('No locked')

    def test_unofficial_download(self):
        self.entry['url'] = 'https://example.com/cli'
        self.assert_rejected('official Dockpipe')


if __name__ == '__main__':
    unittest.main()
