"""Failure evidence must identify actual bytes and never retry a native test."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

PACKAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE))
from tooling import windows_test_diagnostics as diagnostics


class DiagnosticTests(unittest.TestCase):
    def write_pe(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        data = bytearray(128)
        data[:2] = b'MZ'
        struct.pack_into('<I', data, 0x3c, 64)
        data[64:68] = b'PE\0\0'
        struct.pack_into('<H', data, 68, 0x8664)
        path.write_bytes(data)

    def test_selected_import_graph_and_exact_test_bytes_are_retained_without_execution(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build, gio, qt = (root / name for name in ['build', 'gio', 'qt'])
            for name in diagnostics.TEST_NAMES:
                self.write_pe(build / name)
            for name in ['libgio-2.0-0.dll', 'libglib-2.0-0.dll']:
                self.write_pe(gio / 'bin' / name)
            self.write_pe(qt / 'bin/Qt6Test.dll')
            inspector = root / 'tools/dumpbin.exe'
            self.write_pe(inspector)
            destination = root / 'evidence'
            environment = {'DOCKPIPE_VM_DISPLAY_INSPECTOR': str(inspector), 'PATH': 'selected;path',
                           'DOCKPIPE_VM_BUILD_DIAGNOSTICS': str(destination)}

            def inspect(arguments, **options):
                self.assertEqual(arguments[0], str(inspector))
                name = Path(arguments[-1]).name
                if '/imports' in arguments:
                    return SimpleNamespace(stdout='fixture symbol import from ' + name, stderr='')
                imports = ['KERNEL32.dll']
                if name in diagnostics.TEST_NAMES:
                    imports += ['Qt6Test.dll']
                if name.startswith('qmp-'):
                    imports += ['libgio-2.0-0.dll', 'vcruntime140_threads.dll']
                elif name.startswith('native-windows') or name == 'libgio-2.0-0.dll':
                    imports += ['libglib-2.0-0.dll']
                return SimpleNamespace(stdout='\n'.join('    ' + value for value in imports), stderr='')

            with patch.object(diagnostics.subprocess, 'run', side_effect=inspect) as run, patch('builtins.print'):
                evidence = diagnostics.diagnose_tests(build, gio, qt, environment)
            self.assertEqual(evidence['effective_path'], 'selected;path')
            self.assertEqual(evidence['test_retries'], 0)
            self.assertFalse(evidence['dll_execution_or_copy'])
            self.assertEqual(run.call_count, 8)
            self.assertEqual(set(evidence['test_symbol_imports']), set(diagnostics.TEST_NAMES[:2]))
            self.assertEqual(set(path.name for path in destination.iterdir()),
                             {*diagnostics.TEST_NAMES, 'windows-test-evidence.json'})
            for name in diagnostics.TEST_NAMES:
                self.assertEqual((destination / name).read_bytes(), (build / name).read_bytes())
            self.assertEqual(json.loads((destination / 'windows-test-evidence.json').read_bytes()), evidence)

    @unittest.skipUnless(shutil.which('cmake') and os.name != 'nt', 'Host CMake and executable shell fixtures required')
    def test_cmake_runner_executes_once_and_reports_original_child_exit(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for expected in [0, 23]:
                marker = root / ('count-' + str(expected))
                executable = root / ('fixture ' + str(expected))
                executable.write_text('#!/bin/sh\nprintf x >> "' + str(marker) + '"\nexit ' + str(expected) + '\n')
                executable.chmod(0o755)
                result = subprocess.run([shutil.which('cmake'), '-DPROGRAM=' + str(executable), '-P',
                                         str(PACKAGE / 'tooling/RunNativeTest.cmake')],
                                        capture_output=True, text=True, timeout=15)
                self.assertEqual(marker.read_text(), 'x')
                if expected == 0:
                    self.assertEqual(result.returncode, 0, result.stderr)
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('Native test child exit status: 23', result.stderr)


if __name__ == '__main__':
    unittest.main()
