import hashlib
import io
import os
import sys
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[1] / 'assets/scripts'
sys.path.insert(0, str(SCRIPTS))
import framework_extract as extraction


@unittest.skipIf(os.name == 'nt', 'Darwin framework decoder is not a Windows recipe dependency')
class FrameworkExtractionTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.staging = self.root / 'tree'
        self.staging.mkdir()
        self.tree = extraction.FrameworkTree(self.staging)

    def file(self, name, data=b'framework-bytes', **kwargs):
        incoming = io.BytesIO(data)
        self.tree.add(extraction.Member(name, 'file', len(data), 0o755, **kwargs), incoming.read)

    def link(self, name, target):
        self.tree.add(extraction.Member(name, 'symlink', 0, 0o777, target), lambda size: b'')

    def test_qt_framework_alias_chain_is_preserved_and_resolves_contained(self):
        self.file('lib/QtCore.framework/Versions/A/QtCore')
        self.file('lib/QtCore.framework/Versions/A/Headers/qglobal.h')
        self.link('lib/QtCore.framework/Versions/Current', 'A')
        self.link('lib/QtCore.framework/QtCore', 'Versions/Current/QtCore')
        self.link('lib/QtCore.framework/Headers', 'Versions/Current/Headers')
        self.assertFalse((self.staging / 'lib/QtCore.framework/Headers').is_symlink())
        receipt = self.tree.finish()
        self.assertEqual(receipt['aliases'], 3)
        self.assertEqual((self.staging / 'lib/QtCore.framework/Headers/qglobal.h').read_bytes(), b'framework-bytes')
        self.assertTrue((self.staging / 'lib/QtCore.framework/QtCore').is_symlink())

    def test_contained_parent_relative_alias_is_supported(self):
        self.file('lib/libQtCore.6.dylib')
        self.link('bin/qtcore', '../lib/libQtCore.6.dylib')
        self.tree.finish()
        self.assertEqual((self.staging / 'bin/qtcore').read_bytes(), b'framework-bytes')

    def test_escape_absolute_windows_and_control_targets_are_rejected(self):
        for target in ('../outside', '/etc/passwd', 'C:/file', 'a\\b', 'bad\nname', ''):
            with self.subTest(target=target), self.assertRaises(ValueError):
                extraction.link_destination('alias', target)

    def test_cycles_dangling_and_recursive_directory_aliases_create_no_links(self):
        for links in ([('a', 'b'), ('b', 'a')], [('a', 'missing')], [('folder/link', '..')]):
            directory = self.root / str(len(list(self.root.iterdir())))
            directory.mkdir()
            tree = extraction.FrameworkTree(directory)
            for name, target in links:
                tree.add(extraction.Member(name, 'symlink', 0, 0, target), lambda size: b'')
            with self.assertRaises(ValueError):
                tree.finish()
            self.assertFalse(any(path.is_symlink() for path in directory.rglob('*')))

    def test_alias_parent_entry_is_rejected_in_either_archive_order(self):
        self.link('alias', 'elsewhere')
        with self.assertRaises(ValueError):
            self.file('alias/file')
        self.file('directory/file')
        with self.assertRaises(ValueError):
            self.link('directory', 'elsewhere')

    def test_case_unicode_aliases_duplicates_and_special_files_fail(self):
        self.file('Upper')
        self.file('caf\u00e9')
        for name, kind in (('upper', 'file'), ('Upper', 'file'), ('cafe\u0301', 'file'),
                           ('hard', 'hardlink'), ('device', 'special')):
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.tree.add(extraction.Member(name, kind, 0, 0), lambda size: b'')

    def test_expanded_member_count_and_payload_lengths_are_bounded(self):
        self.tree.maximum_files = 1
        self.file('one')
        with self.assertRaises(ValueError):
            self.file('two')
        for size, content in ((1, b'ab'), (2, b'a')):
            tree = extraction.FrameworkTree(self.staging)
            with self.subTest(size=size), self.assertRaises(ValueError):
                tree.add(extraction.Member('payload' + str(size), 'file', size, 0), io.BytesIO(content).read)
        tree = extraction.FrameworkTree(self.staging, maximum_bytes=2)
        with self.assertRaises(ValueError):
            tree.add(extraction.Member('large', 'file', 3, 0), io.BytesIO(b'abc').read)

    def test_link_hops_links_and_cancellation_are_bounded(self):
        self.file('final')
        for index in range(extraction.MAX_LINK_HOPS + 1):
            self.link('link' + str(index), 'final' if index == extraction.MAX_LINK_HOPS else 'link' + str(index + 1))
        with self.assertRaises(ValueError):
            self.tree.finish()
        cancelled = extraction.FrameworkTree(self.staging, cancelled=lambda: True)
        with self.assertRaisesRegex(ValueError, 'cancelled'):
            cancelled.add(extraction.Member('cancelled', 'directory', 0, 0), lambda size: b'')
        with mock.patch.object(extraction, 'MAX_LINKS', 0):
            with self.assertRaises(ValueError):
                self.link('another', 'final')

    def archive(self):
        path = self.root / 'framework.tar.gz'
        with tarfile.open(path, 'w:gz') as archive:
            regular = tarfile.TarInfo('Framework/Versions/A/binary')
            regular.size = 3
            regular.mode = 0o755
            archive.addfile(regular, io.BytesIO(b'abc'))
            for name, target in (('Framework/Versions/Current', 'A'), ('Framework/binary', 'Versions/Current/binary')):
                entry = tarfile.TarInfo(name)
                entry.type = tarfile.SYMTYPE
                entry.linkname = target
                archive.addfile(entry)
        return {'path': path, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'size_bytes': path.stat().st_size}

    def test_real_libarchive_read_only_decoder_and_atomic_admission(self):
        entry = self.archive()
        result = extraction.extract_locked([entry], self.root / 'installed')
        self.assertFalse(result['native_execution'])
        self.assertEqual((self.root / 'installed/Framework/binary').read_bytes(), b'abc')
        self.assertTrue((self.root / 'installed/Framework/binary').is_symlink())
        with self.assertRaises(ValueError):
            extraction.extract_locked([entry], self.root / 'installed')
        changed = dict(entry, sha256='0' * 64)
        with mock.patch.object(extraction, 'LibArchive') as reader:
            with self.assertRaises(ValueError):
                extraction.extract_locked([changed], self.root / 'wrong')
            reader.assert_not_called()
        self.assertFalse((self.root / 'wrong').exists())

    def test_failed_archive_transaction_does_not_leave_output(self):
        entry = self.archive()
        with self.assertRaises(ValueError):
            extraction.extract_locked([entry], self.root / 'cancelled', cancelled=lambda: True)
        self.assertFalse((self.root / 'cancelled').exists())
        self.assertFalse(list(self.root.glob('.framework-stage-*')))


if __name__ == '__main__':
    unittest.main()
