"""Producer admission tests; no native binary execution or toolchain discovery."""
import hashlib
import json
import os
import subprocess
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tooling.broker_closure import broker_inputs, bundle_broker
from tooling.runtime_closure import windows_closure
from tooling.pinned_licenses import pinned_license_files


class RuntimeClosureTest(unittest.TestCase):
    def test_missing_bottle_license_uses_only_exact_pinned_upstream_version(self):
        files, provenance = pinned_license_files('glib', '2.90.1')
        self.assertEqual(len(files), 11)
        self.assertEqual(provenance['source_sha256'],
                         '93c941aa17d5eb1d53fe838365f29a8b4e539c222a256d974ec8f30fc413e396')
        with self.assertRaisesRegex(ValueError, 'No pinned upstream license profile'):
            pinned_license_files('glib', '2.90.2')

    def test_windows_runtime_copies_package_dll_and_declares_host_crt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            gio = root / 'gio'
            (gio / 'bin').mkdir(parents=True)
            (gio / 'share/licenses/dbus').mkdir(parents=True)
            (gio / 'share/licenses/dbus/COPYING').write_text('test license')
            (gio / 'bin/libgio-2.0-0.dll').write_bytes(b'test dependency')
            module = root / 'module.dll'
            module.touch()
            inspector = root / 'dumpbin.exe'
            inspector.touch()
            output = root / 'output'
            output.mkdir()
            imports = '    MSVCP140.dll\n    VCRUNTIME140_1.dll\n    Qt6Core.dll\n    libgio-2.0-0.dll\n'
            with patch('tooling.runtime_closure.capture', return_value=imports):
                records = windows_closure(module, output, gio, root / 'qt', inspector)
            self.assertEqual([record['name'] for record in records], ['libgio-2.0-0.dll'])
            self.assertFalse((output / 'MSVCP140.dll').exists())
            with patch('tooling.runtime_closure.capture', return_value='    VCRUNTIME999.dll\n'):
                with self.assertRaisesRegex(ValueError, 'Unresolved native runtime DLL'):
                    windows_closure(module, output, gio, root / 'qt', inspector)

    @unittest.skipIf(os.name == 'nt', 'POSIX executable-mode contract')
    def test_linux_broker_uses_archive_executable_seam_without_path_lookup(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / 'input-broker'
            binary.write_text('#!/bin/sh\nprintf "private-broker-ok\\n"\n')
            binary.chmod(0o755)
            license_file = root / 'COPYING'
            license_file.write_text('test license')
            record = {'version': '1.16.2', 'licenses': [{'path': str(license_file)}]}
            output = root / 'display'
            bundle_broker(binary, record, output, 'Linux', None, None, None, None,
                          lambda path: {}, lambda path: None)
            executable = output / 'tooling/bin/dbus-daemon'
            self.assertEqual(executable.stat().st_mode & 0o777, 0o755)
            self.assertTrue((executable.parent / 'dbus-daemon.real').stat().st_mode & 0o111)
            self.assertFalse((output / 'bin').exists())
            result = subprocess.run([str(executable)], env={'PATH': ''}, check=True,
                                    capture_output=True, text=True, timeout=3)
            self.assertEqual(result.stdout, 'private-broker-ok\n')

    def test_broker_requires_exact_binary_and_license_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / 'dbus-daemon'
            binary.write_bytes(b'qualified broker')
            license_file = root / 'COPYING'
            license_file.write_text('test license')
            record = {'schema': 1, 'version': '1.16.2',
                      'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                      'source_url': 'https://dbus.freedesktop.org/releases/dbus/dbus-1.16.2.tar.xz',
                      'source_sha256': 'a' * 64,
                      'licenses': [{'path': str(license_file),
                                    'sha256': hashlib.sha256(license_file.read_bytes()).hexdigest()}]}
            descriptor = root / 'provenance.json'
            descriptor.write_text(json.dumps(record))
            environment = {'DOCKPIPE_VM_DBUS_DAEMON_BIN': str(binary),
                           'DOCKPIPE_VM_DBUS_PROVENANCE': str(descriptor)}
            with patch.dict(os.environ, environment):
                self.assertEqual(broker_inputs()[0], binary.resolve())
                license_file.write_text('changed')
                with self.assertRaisesRegex(ValueError, 'license differs'):
                    broker_inputs()
                binary.write_bytes(b'changed')
                with self.assertRaisesRegex(ValueError, 'binary differs'):
                    broker_inputs()


if __name__ == '__main__':
    unittest.main()
