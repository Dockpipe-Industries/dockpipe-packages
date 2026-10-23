"""Native license provenance resolves one exact installed architecture."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
PACKAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE))
specification = importlib.util.spec_from_file_location('display_producer', PACKAGE / 'build.py')
producer = importlib.util.module_from_spec(specification)
specification.loader.exec_module(producer)


class ProducerOwnershipTest(unittest.TestCase):
    def test_architecture_is_preserved_for_exact_package_version(self):
        path = Path('/usr/lib/x86_64-linux-gnu/libexpat.so.1')
        result = subprocess.CompletedProcess([], 0, 'libexpat1:amd64: ' + str(path) + '\n')
        with patch.object(producer.subprocess, 'run', return_value=result):
            self.assertEqual(producer.owning_package(path), 'libexpat1:amd64')
        with patch.object(producer, 'owning_package', return_value='libexpat1:amd64'), \
                patch.object(Path, 'is_file', return_value=True), \
                patch.object(producer.subprocess, 'check_output', return_value='2.4.7-1ubuntu0.9') as query:
            package, version, license_path = producer.package_license(path)
        self.assertEqual(package, 'libexpat1:amd64')
        self.assertEqual(version, '2.4.7-1ubuntu0.9')
        self.assertEqual(license_path, Path('/usr/share/doc/libexpat1/copyright'))
        self.assertEqual(query.call_args.args[0][-1], 'libexpat1:amd64')


if __name__ == '__main__':
    unittest.main()
