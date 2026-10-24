import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

SCRIPTS = Path(__file__).resolve().parents[1] / 'assets/scripts'
sys.path.insert(0, str(SCRIPTS))
import private_pkgconfig


@unittest.skipUnless(shutil.which('pkg-config'), 'Requires a real pkg-config metadata parser')
class PrivatePkgconfigTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.cellar = Path(self.temporary.name) / 'Cellar'
        self.cellar.mkdir()
        self.pkgconfig = Path(shutil.which('pkg-config')).resolve()
        self.environment = dict(os.environ, PKG_CONFIG_PATH='', PKG_CONFIG_SYSROOT_DIR='')
        self.sources = json.loads((Path(__file__).parent / 'fixtures/macos-pkgconfig-bottles.json').read_text())
        recipe = json.loads((Path(__file__).resolve().parents[4] / 'release/native-inputs/darwin-arm64.json').read_text())
        pins = {entry['id']: entry for entry in recipe['archives']}
        directories = []
        self.files = {}
        for source in self.sources:
            self.assertEqual(source['archive_sha256'], pins[source['package']]['asset']['sha256'])
            for directory in source['header_directories']:
                (self.cellar / directory).mkdir(parents=True, exist_ok=True)
            for record in source['files']:
                content = record['content']
                self.assertEqual(hashlib.sha256(content.encode()).hexdigest(), record['sha256'])
                for package, entry in pins.items():
                    content = content.replace('@@HOMEBREW_PREFIX@@/opt/' + package,
                                              str(self.cellar / package / entry['version']))
                content = content.replace('@@HOMEBREW_CELLAR@@', str(self.cellar))
                path = self.cellar / record['path']
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
                self.files[path.name] = path
                if str(path.parent) not in directories:
                    directories.append(str(path.parent))
        (self.cellar / 'gettext/1.0/lib').mkdir(parents=True)
        (self.cellar / 'gettext/1.0/include').mkdir()
        self.environment['PKG_CONFIG_LIBDIR'] = os.pathsep.join(directories)

    def admit(self):
        return private_pkgconfig.admit_gio_graph(self.pkgconfig, self.environment, self.cellar)

    def test_actual_pinned_bottle_metadata_closes_full_gio_graph(self):
        receipt = self.admit()
        self.assertEqual(receipt['version'], '2.90.1')
        self.assertEqual(len(receipt['metadata']), 8)

    def test_each_missing_actual_transitive_dependency_fails(self):
        for name in ('zlib.pc', 'libffi.pc', 'libpcre2-8.pc'):
            path = self.files[name]
            original = path.read_bytes()
            path.unlink()
            try:
                with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'graph failed'):
                    self.admit()
            finally:
                path.write_bytes(original)

    def test_malformed_metadata_and_wrong_gio_version_fail(self):
        path = self.files['gio-unix-2.0.pc']
        original = path.read_text()
        for content in ('bad\0metadata', original.replace('Version: 2.90.1', 'Version: 9.0.0'), 'Name: invalid\n'):
            path.write_text(content)
            with self.subTest(content=content[:20]), self.assertRaises(ValueError):
                self.admit()

    def test_escaping_metadata_directory_flags_and_alias_fail(self):
        original = self.files['gio-unix-2.0.pc'].read_text()
        self.files['gio-unix-2.0.pc'].write_text(original.replace('Cflags:', 'Cflags: -I' + str(self.cellar.parent)))
        with self.assertRaisesRegex(ValueError, 'flags escape'):
            self.admit()
        self.files['gio-unix-2.0.pc'].write_text(original)
        outside = self.cellar.parent / 'external.pc'
        outside.write_text(original)
        self.files['gio-unix-2.0.pc'].unlink()
        self.files['gio-unix-2.0.pc'].symlink_to(outside)
        with self.assertRaisesRegex(ValueError, 'metadata escapes'):
            self.admit()
        self.environment['PKG_CONFIG_LIBDIR'] = '/usr/lib/pkgconfig'
        with self.assertRaisesRegex(ValueError, 'directory escapes'):
            self.admit()

    def test_ambient_search_is_never_admitted(self):
        self.environment['PKG_CONFIG_PATH'] = '/usr/lib/pkgconfig'
        with self.assertRaisesRegex(ValueError, 'disable ambient'):
            self.admit()
