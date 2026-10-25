"""Source-independent native execution is mandatory and cannot claim network denial."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'assets/scripts'))
import catalog_consumer
from catalog_consumer_gate import require_runtime_capability
from catalog_transaction import PublicationStopped


class RuntimeCapabilitiesTests(unittest.TestCase):
    def test_real_command_failure_cannot_be_reported_as_acceptance(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(catalog_consumer, 'run_cli', side_effect=PublicationStopped('old CLI unsupported')):
                with self.assertRaisesRegex(PublicationStopped, 'unsupported'):
                    catalog_consumer.verify_runtime_capabilities(Path('/selected/dockpipe'), 'a' * 64, 'test', root, {})

    def test_capabilities_bind_exact_cli_helpers_and_command_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            selected_cli = root / 'selected-cli'
            selected_cli.write_bytes(b'fixture')
            helpers = {}
            for language in ('shell', 'powershell', 'python'):
                helpers[language] = root / (language + '.helper')
                helpers[language].write_text(language)
            calls = []
            def invoke(command, arguments, project, environment, label):
                calls.append((command, arguments, environment))
                if arguments == ['init']:
                    target = project / 'workflows/example/config.yml'
                    target.parent.mkdir(parents=True)
                    target.write_text('kind: host\n')
                    return b'initialized'
                if '--workflow' in arguments:
                    return b'Hello from Dockpipe'
                if arguments[:2] == ['sdk', 'path']:
                    return str(helpers[arguments[3]]).encode()
                self.assertEqual(command, Path(sys.executable))
                self.assertIn('DOCKPIPE_BIN', arguments[1])
                return b'{"scope":"fixture"}'
            with patch.object(catalog_consumer, 'run_cli', side_effect=invoke):
                receipt = catalog_consumer.verify_runtime_capabilities(selected_cli, 'a' * 64, 'test', root,
                    {'DOCKPIPE_BIN': str(selected_cli), 'DOCKPIPE_GLOBAL_ROOT': str(root / 'global')})
            require_runtime_capability(receipt, {'sha256': 'a' * 64, 'version': 'test'})
            self.assertEqual(len(calls), 6)
            self.assertFalse(receipt['network_denial_claimed'])
            self.assertEqual(receipt['helpers']['python']['sha256'], hashlib.sha256(b'python').hexdigest())
            receipt['cli_sha256'] = 'b' * 64
            with self.assertRaisesRegex(PublicationStopped, 'binding'):
                require_runtime_capability(receipt, {'sha256': 'a' * 64, 'version': 'test'})

    def test_sdk_resource_outside_private_consumer_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def invoke(_command, arguments, project, _environment, _label):
                if arguments == ['init']:
                    target = project / 'workflows/example/config.yml'
                    target.parent.mkdir(parents=True)
                    target.write_text('kind: host\n')
                    return b'initialized'
                if '--workflow' in arguments:
                    return b'Hello from Dockpipe'
                return str(Path(sys.executable).resolve()).encode()
            with patch.object(catalog_consumer, 'run_cli', side_effect=invoke):
                with self.assertRaisesRegex(PublicationStopped, 'escaped'):
                    catalog_consumer.verify_runtime_capabilities(Path('/selected/cli'), 'a' * 64, 'test', root, {})

    def test_success_boolean_cannot_replace_native_command_and_helper_evidence(self):
        with self.assertRaises(PublicationStopped):
            require_runtime_capability({'success': True}, {'sha256': 'a' * 64, 'version': 'test'})


if __name__ == '__main__':
    unittest.main()
