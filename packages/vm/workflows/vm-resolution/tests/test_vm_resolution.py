"""Exercise the workflow boundary without contacting a guest or changing a display."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parent.parent / 'assets/scripts/vm-resolution.py'
specification = importlib.util.spec_from_file_location('vm_resolution', SCRIPT)
resolution = importlib.util.module_from_spec(specification)
specification.loader.exec_module(resolution)


class ResolutionWorkflowTest(unittest.TestCase):
    def test_selected_surface_and_explicit_run_id_reach_installed_manager(self):
        for explicit in ('', 'cli-run'):
            with self.subTest(explicit=explicit), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                program = root / 'dockpipe'
                program.touch()
                assets = root / 'resolver assets'
                suffix = '.exe' if os.name == 'nt' else ''
                manager = assets / 'tooling/bin' / ('dockpipe-vm-manager' + suffix)
                manager.parent.mkdir(parents=True)
                manager.touch()
                environment = {
                    'DOCKPIPE_BIN': str(program),
                    'DOCKPIPE_WORKDIR': str(root),
                    'DOCKPIPE_SURFACE_RESOURCE_ID': 'selected-run',
                    'DOCKPIPE_VM_TARGET_RUN_ID': explicit,
                    'DOCKPIPE_VM_DISPLAY_WIDTH': '1920',
                    'DOCKPIPE_VM_DISPLAY_HEIGHT': '1080',
                }
                lookup = subprocess.CompletedProcess([], 0, str(assets).encode())
                action = subprocess.CompletedProcess([], 23)
                with patch.dict(os.environ, environment, clear=True):
                    with patch.object(resolution.subprocess, 'run', side_effect=[lookup, action]) as run:
                        self.assertEqual(resolution.main(), 23)
                        self.assertEqual(run.call_args_list[0].args[0], [
                            str(program), 'scope', 'resolver', 'qemu', 'assets-dir',
                            '--workdir', str(root),
                        ])
                        self.assertEqual(run.call_args_list[1].args[0], [str(manager), 'resolution'])
                        self.assertNotIn('env', run.call_args_list[1].kwargs)
                        self.assertEqual(os.environ['DOCKPIPE_SURFACE_RESOURCE_ID'], 'selected-run')
                        self.assertEqual(os.environ['DOCKPIPE_VM_TARGET_RUN_ID'], explicit)

    def test_missing_or_invalid_target_stops_before_tool_lookup(self):
        for target in ('', '../run', 'run;exit'):
            with self.subTest(target=target):
                with patch.dict(os.environ, {'DOCKPIPE_SURFACE_RESOURCE_ID': target}, clear=True):
                    with patch.object(resolution.subprocess, 'run') as run:
                        with self.assertRaises(ValueError):
                            resolution.main()
                        run.assert_not_called()


if __name__ == '__main__':
    unittest.main()
