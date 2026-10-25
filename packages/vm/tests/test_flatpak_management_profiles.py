"""Management workflows declare their own platform and resolver contracts."""
import json
from pathlib import Path
import unittest

import yaml


PACKAGE = Path(__file__).resolve().parents[1]


class ManagementFlatpakProfiles(unittest.TestCase):
    def test_both_workflows_explicitly_require_the_native_qemu_resolver(self):
        for name in ('vm-manage', 'vm-resolution'):
            with self.subTest(workflow=name):
                workflow = PACKAGE / 'workflows' / name
                manifest = yaml.safe_load((workflow / 'package.yml').read_text())
                self.assertEqual(manifest['kind'], 'workflow')
                self.assertEqual(manifest['name'], name)
                self.assertEqual(manifest['requires_resolvers'], ['qemu'])
                self.assertEqual(manifest['version'], '1.3.8')
                self.assertEqual(manifest['min_dockpipe_version'], '0.6.29-0')
                self.assertEqual(manifest['platforms'], ['windows', 'deb', 'macos', 'flatpak'])

    def test_host_python_steps_do_not_activate_a_vm_boot_profile(self):
        for name in ('vm-manage', 'vm-resolution'):
            with self.subTest(workflow=name):
                config = yaml.safe_load((PACKAGE / 'workflows' / name / 'config.yml').read_text())
                self.assertEqual(config['platforms'], ['windows', 'deb', 'macos', 'flatpak'])
                self.assertIs(config['docker_preflight'], False)
                self.assertNotIn('inject', config)
                self.assertNotIn('resolver', config)
                self.assertNotIn('runtime', config)
                self.assertEqual(len(config['steps']), 1)
                step = config['steps'][0]
                self.assertEqual(step['kind'], 'host')
                self.assertEqual(step['run'], 'assets/scripts/' + name + '.py')
                self.assertNotIn('resolver', step)
                self.assertNotIn('runtime', step)

    def test_each_profile_owns_its_exact_workflow_and_required_payload(self):
        for name in ('vm-manage', 'vm-resolution'):
            with self.subTest(workflow=name):
                profile = json.loads((PACKAGE / 'workflows' / name / 'assets/flatpak/package.json').read_text())
                self.assertEqual(profile['schema'], 1)
                self.assertEqual(profile['artifacts'], [{'kind': 'workflow', 'name': name}])
                self.assertEqual(profile['tools'], [])
                self.assertEqual(profile['host_commands'], [])
                expected = ['config.yml', 'assets/scripts/' + name + '.py']
                if name == 'vm-resolution':
                    expected.append('models/VmResolutionConfig.pipe')
                self.assertEqual(profile['required_files'], expected)


if __name__ == '__main__':
    unittest.main()
