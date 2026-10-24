import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


hook = module('mac_source_hook', ROOT / 'assets/scripts/build-source.py')
runner = module('mac_test_hook', ROOT / 'tests/run.py')


class MacContractTest(unittest.TestCase):
    def test_native_surface_exposes_mac_launch_but_not_unqualified_clipboard(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'surface.json'
            actions = ['start-windows', 'clipboard-start', 'clipboard-stop', 'resolution', 'shutdown']
            path.write_text(json.dumps({'actions': [{'id': name} for name in actions]}))
            hook.update_launcher(path, 'darwin', 'dockpipe-qemu-launch')
            result = json.loads(path.read_text())
            self.assertEqual([item['id'] for item in result['actions']], ['start-windows', 'resolution', 'shutdown'])
            launch = result['actions'][0]['launch_environment']['macos']
            self.assertEqual(launch['DOCKPIPE_VM_QEMU_BIN'],
                             {'asset': 'assets/tooling/bin/dockpipe-qemu-launch'})
            self.assertEqual(hook.LIBRARIES['darwin'], 'libdockpipe-vm-display.dylib')

    def test_linux_launch_environment_contract_is_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'surface.json'
            path.write_text(json.dumps({'actions': [{'id': 'start-windows'}]}))
            hook.update_launcher(path, 'linux', 'dockpipe-qemu-launch')
            action = json.loads(path.read_text())['actions'][0]
            self.assertEqual(set(action['launch_environment']), {'linux'})
            self.assertEqual(action['launch_environment']['linux']['DOCKPIPE_VM_QEMU_BIN'],
                             {'asset': 'assets/tooling/bin/dockpipe-qemu-launch'})

    def test_mac_tests_only_native_root_manager_and_owning_native_source_hook(self):
        with tempfile.TemporaryDirectory() as temporary:
            cli = Path(temporary) / 'dockpipe'
            cli.write_bytes(b'fixture')
            cli.chmod(0o755)
            environment = {name: str(cli) for name in (
                'DOCKPIPE_BIN', 'DOCKPIPE_VM_DISPLAY_SDK_PREFIX', 'DOCKPIPE_VM_HOST_QT_PREFIX',
                'DOCKPIPE_GIO_PREFIX', 'DOCKPIPE_VM_DBUS_DAEMON_BIN', 'DOCKPIPE_VM_DBUS_PROVENANCE',
                'DOCKPIPE_VM_GUEST_TOOLS_ARCHIVE', 'DOCKPIPE_VM_GUEST_TOOLS_DESCRIPTOR', 'DOCKPIPE_GO_SDK_INPUTS')}
            commands = runner.test_commands(ROOT, 'Darwin', environment)
            self.assertEqual(len(commands), 3)
            self.assertIn('test_runtime_compatibility.py', commands[0][1][-1])
            self.assertIn('./packages/vm/management/...', commands[1][1])
            self.assertEqual(commands[2][1][-2:], ['--only', 'vm'])
            self.assertNotIn(str(ROOT / 'tools'), str(commands))
            environment.pop('DOCKPIPE_VM_DBUS_DAEMON_BIN')
            with self.assertRaises(ValueError):
                runner.test_commands(ROOT, 'Darwin', environment)

    def test_linux_full_legacy_suite_is_not_replaced_by_mac_tests(self):
        self.assertEqual(runner.test_commands(ROOT, 'Linux', {}),
                         [(ROOT, ['bash', str(ROOT / 'tests/run.sh')])])
        with self.assertRaises(ValueError):
            runner.test_commands(ROOT, 'FreeBSD', {})

    def test_windows_keeps_its_existing_explicit_portable_matrix(self):
        commands = runner.test_commands(ROOT, 'Windows', {})
        import vm_go
        self.assertEqual([arguments for directory, arguments in commands[:-1]], vm_go.test_arguments('Windows'))
        self.assertIn('test_runtime_compatibility.py', commands[-1][1][-1])
        self.assertTrue(all(directory == ROOT / 'tools' for directory, arguments in commands[:-1]))
        self.assertTrue(all('./...' not in arguments for directory, arguments in commands))

    def test_only_supported_metadata_is_extended(self):
        self.assertIn('version: 1.3.7\n', (ROOT / 'package.yml').read_text())
        self.assertIn('min_dockpipe_version: 0.6.29-0\n', (ROOT / 'package.yml').read_text())
        self.assertIn('platforms: [windows, deb, macos]', (ROOT / 'package.yml').read_text())
        self.assertIn('script: assets/scripts/build-source.py', (ROOT / 'package.yml').read_text())
        self.assertIn('script: tests/run.py', (ROOT / 'package.yml').read_text())
        self.assertIn('platforms: [windows, deb, macos, flatpak]', (ROOT / 'workflows/vm-resolution/config.yml').read_text())
        management = (ROOT / 'workflows/vm-manage/config.yml').read_text()
        self.assertIn('platforms: [windows, deb, macos, flatpak]', management)
        self.assertIn('run: assets/scripts/vm-manage.py', management)
        self.assertNotIn('cmd:', management)


if __name__ == '__main__':
    unittest.main()
