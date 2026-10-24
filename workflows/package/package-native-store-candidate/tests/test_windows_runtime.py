"""Failure admission for the exact host CRT; no system-directory fallback."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import test_native_store
import windows_runtime


class WindowsRuntimeTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.directory = self.root / 'VC/Redist/exact/x64/CRT'
        self.directory.mkdir(parents=True)
        records = []
        for name in ('msvcp140.dll', 'vcruntime140_threads.dll'):
            data = ('fixture ' + name).encode()
            (self.directory / name).write_bytes(data)
            records.append({'name': name, 'path': 'VC/Redist/exact/x64/CRT/' + name,
                            'sha256': hashlib.sha256(data).hexdigest(), 'size_bytes': len(data),
                            'version': '14.51.36247.0'})
        self.expected = {'version': '14.51.36247.0', 'sha256': 'a' * 64,
                         'directory': 'VC/Redist/exact/x64/CRT', 'files': records}
        self.receipt = {'version': self.expected['version'], 'source_sha256': self.expected['sha256'],
                        'directory': self.expected['directory'],
                        'files': {record['name']: record for record in records}}

    def test_exact_runtime_staged_only_into_disposable_probe(self):
        (self.directory / 'unlisted.dll').write_bytes(b'not admitted')
        destination = self.root / 'probe'
        destination.mkdir()
        windows_runtime.stage_probe_runtime(self.root, self.receipt, self.expected, destination)
        self.assertEqual(sorted(path.name for path in destination.iterdir()), sorted(self.receipt['files']))
        with self.assertRaisesRegex(ValueError, 'already exists'):
            windows_runtime.stage_probe_runtime(self.root, self.receipt, self.expected, destination)

    def test_missing_threads_corruption_and_symlink_fail_closed(self):
        path = self.directory / 'vcruntime140_threads.dll'
        original = path.read_bytes()
        for mutation in ('missing', 'corrupt', 'symlink'):
            with self.subTest(mutation=mutation):
                path.unlink()
                if mutation == 'corrupt':
                    path.write_bytes(b'x' * len(original))
                elif mutation == 'symlink':
                    target = self.root / 'outside.dll'
                    target.write_bytes(original)
                    path.symlink_to(target)
                with self.assertRaisesRegex(ValueError, 'runtime bytes differ'):
                    windows_runtime.validate_runtime(self.root, self.receipt, self.expected)
                if path.exists() or path.is_symlink():
                    path.unlink()
                path.write_bytes(original)

    def test_old_runtime_missing_record_extra_record_or_escape_refused(self):
        mutations = [dict(version='14.44.0.0'), dict(source_sha256='b' * 64), dict(directory='../System32')]
        for fields in mutations:
            changed = dict(self.receipt, **fields)
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                windows_runtime.validate_runtime(self.root, changed, self.expected)
        for name in ('missing', 'unknown-future-crt.dll'):
            changed = copy.deepcopy(self.receipt)
            if name == 'missing':
                del changed['files']['vcruntime140_threads.dll']
            else:
                changed['files'][name] = {}
            with self.subTest(name=name), self.assertRaises(ValueError):
                windows_runtime.validate_runtime(self.root, changed, self.expected)

    def test_official_desktop_inventory_binds_threads_and_not_onecore(self):
        source = Path(__file__).resolve().parents[4]
        descriptor = json.loads((source / 'release/native-compiler/windows-hosted-19.51.json').read_bytes())
        runtime = descriptor['desktop_runtime']
        self.assertEqual(runtime['package'], 'Microsoft.VC.14.51.CRT.Redist.X64.base')
        self.assertEqual(len(runtime['files']), 10)
        records = {record['name']: record for record in runtime['files']}
        self.assertEqual(records['vcruntime140_threads.dll']['sha256'],
                         '40f39e8cee5f5a531b4010e18b807e8fe265b908dc58f959ecb7ebc6ea6cb11a')
        self.assertEqual({record['version'] for record in records.values()}, {'14.51.36247.0'})
