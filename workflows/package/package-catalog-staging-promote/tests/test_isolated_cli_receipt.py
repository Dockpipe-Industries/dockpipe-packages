"""Reviewed raw isolated proof must bind the actual public binary and commands."""
import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'assets/scripts'))
from catalog_driver import admit_isolated_cli_qualification
from catalog_transaction import PublicationStopped


class IsolatedCliReceiptTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / 'release/catalog-qualification').mkdir(parents=True)
        self.cli = {'executable_sha256': 'a' * 64, 'version': 'public-fixture-version'}
        self.commands = [{'argv': ['init'], 'exit': 0},
                         {'argv': ['--workdir', '/consumer/project', '--workflow', 'example'], 'exit': 0}]
        self.commands += [{'argv': ['sdk', 'path', '--language', language], 'exit': 0}
                          for language in ('shell', 'powershell', 'python')]
        self.receipt = {'schema': 1, 'cli_sha256': 'a' * 64, 'cli_version': 'public-fixture-version',
                        'cli_source_sha': 'b' * 40, 'network': 'none', 'source_or_sibling_mounts': False,
                        'checks': ['typed offline init', 'execute real host starter', 'three SDK resource languages',
                                   'real Python SDK explicit CLI without PATH CLI']}

    def write(self, name, value):
        relative = 'release/catalog-qualification/' + name
        path = self.root / relative
        path.write_text(json.dumps(value))
        return {'path': relative, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'size_bytes': path.stat().st_size}

    def admit(self):
        self.receipt['command_receipt'] = self.write('commands.json', self.commands)
        reference = self.write('isolated.json', self.receipt)
        return admit_isolated_cli_qualification(reference, self.root, self.cli, 'b' * 40)

    def test_exact_reviewed_public_cli_and_actual_commands_are_required(self):
        self.assertEqual(self.admit()['cli_sha256'], 'a' * 64)
        self.receipt['cli_sha256'] = 'c' * 64
        with self.assertRaisesRegex(PublicationStopped, 'actual public executable'):
            self.admit()

    def test_failed_or_missing_raw_capabilities_reject(self):
        self.commands[-1]['exit'] = 1
        with self.assertRaisesRegex(PublicationStopped, 'failed execution'):
            self.admit()
        self.commands.pop()
        with self.assertRaisesRegex(PublicationStopped, 'omits'):
            self.admit()

    def test_relative_receipt_reference_cannot_escape_owning_public_projection(self):
        with self.assertRaises((ValueError, PublicationStopped)):
            admit_isolated_cli_qualification({'path': '../private.json', 'sha256': 'a' * 64, 'size_bytes': 1},
                                             self.root, self.cli, 'b' * 40)


if __name__ == '__main__':
    unittest.main()
