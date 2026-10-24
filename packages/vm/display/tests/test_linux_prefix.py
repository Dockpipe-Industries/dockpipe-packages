"""Extracted prefix admission never substitutes host files or dpkg ownership."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('linux_prefix', Path(__file__).parents[1] / 'tooling/linux_prefix.py')
prefix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prefix)


class LinuxPrefixTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.library = self.root / 'lib/libgio.so.0'
        self.library.parent.mkdir()
        self.library.write_bytes(b'native library')
        self.license = self.root / 'share/doc/glib/copyright'
        self.license.parent.mkdir(parents=True)
        self.license.write_bytes(b'package copyright')
        owner = {'archive_id':'glib-amd64','package':'libglib2.0-0:amd64','version':'2.72.4'}
        def record(path):
            return {**owner,'path':path.relative_to(self.root).as_posix(),
                    'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'size_bytes':path.stat().st_size}
        self.value = {'schema':1,'prefix':str(self.root),
                      'packages':[{'id':owner['archive_id'],'package':owner['package'],
                                   'version':owner['version'],'archive_sha256':'a'*64}],
                      'libraries':[record(self.library)],'licenses':[record(self.license)]}
        self.receipt = self.root / 'receipt.json'
        self.save()

    def save(self):
        self.receipt.write_text(json.dumps(self.value))

    def test_exact_owner_library_license_and_closure(self):
        selected = prefix.selected({'DOCKPIPE_VM_LINUX_DEPENDENCY_MODE':'prefix',
                                    'DOCKPIPE_VM_LINUX_PREFIX_RECEIPT':str(self.receipt)})
        self.assertEqual(selected.package_license(self.library), ('libglib2.0-0:amd64','2.72.4',self.license))
        selected.closure({'libgio.so.0':self.library})
        with self.assertRaises(ValueError):
            selected.closure({'libgio.so.0':Path('/usr/lib/libgio.so.0')})

    def test_expected_failures_never_fall_back_to_dpkg(self):
        self.assertIsNone(prefix.selected({'DOCKPIPE_VM_LINUX_DEPENDENCY_MODE':'host-dpkg'}))
        for environment in [{'DOCKPIPE_VM_LINUX_DEPENDENCY_MODE':'prefix'},
                            {'DOCKPIPE_VM_LINUX_PREFIX_RECEIPT':str(self.receipt)},
                            {'DOCKPIPE_VM_LINUX_DEPENDENCY_MODE':'other'}]:
            with self.subTest(environment=environment), self.assertRaises(ValueError):
                prefix.selected(environment)
        for field, change in [('path','../../outside'),('archive_id','unowned'),('sha256','b'*64),('size_bytes',True)]:
            value = copy.deepcopy(self.value)
            value['libraries'][0][field] = change
            self.receipt.write_text(json.dumps(value))
            with self.subTest(field=field), self.assertRaises(ValueError):
                prefix.LinuxPrefix(self.receipt)

    def test_duplicate_unknown_fields_and_linked_file_fail(self):
        self.value['libraries'].append(copy.deepcopy(self.value['libraries'][0]))
        self.save()
        with self.assertRaises(ValueError):
            prefix.LinuxPrefix(self.receipt)
        self.value['libraries'].pop()
        self.save()
        content = self.library.read_bytes()
        self.library.unlink()
        target = self.root / 'other'
        target.write_bytes(content)
        self.library.symlink_to(target)
        with self.assertRaises(ValueError):
            prefix.LinuxPrefix(self.receipt)

    def test_changed_to_link_after_admission_never_reads_host_fallback(self):
        selected = prefix.LinuxPrefix(self.receipt)
        content = self.library.read_bytes()
        self.library.unlink()
        other = self.root / 'other-library'
        other.write_bytes(content)
        self.library.symlink_to(other)
        with self.assertRaises(ValueError):
            selected.package_license(self.library)

    def test_changed_bytes_after_initial_admission_are_rejected(self):
        selected = prefix.LinuxPrefix(self.receipt)
        self.library.write_bytes(b'changed')
        with self.assertRaises(ValueError):
            selected.package_license(self.library)


if __name__ == '__main__':
    unittest.main()
