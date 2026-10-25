"""Cross-build the real package-owned Windows service from unrelated roots."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

PACKAGE = Path(__file__).resolve().parents[1]
HOOK = Path('assets/scripts/build-source.py')
SOURCE = Path('guest-services/windows')


class GuestAgentReproducibilityTest(unittest.TestCase):
    def run_command(self, arguments, environment, cwd, timeout=120):
        result = subprocess.run(arguments, cwd=cwd, env=environment,
                                capture_output=True, text=True, timeout=timeout)
        self.assertEqual(result.returncode, 0, (result.stdout + result.stderr)[-8192:])
        self.assertLessEqual(len(result.stdout) + len(result.stderr), 65536)
        return result.stdout

    def test_real_hook_has_stable_module_and_identical_pe_in_distinct_roots(self):
        selected_go = os.environ.get('DOCKPIPE_TEST_GO') or shutil.which('go')
        self.assertIsNotNone(selected_go, 'The pinned Go toolchain must be available')
        go = Path(selected_go).resolve(strict=True)
        expected_toolchain = os.environ.get('DOCKPIPE_TEST_GO_VERSION', 'go1.26.10')
        source_files = [PACKAGE / HOOK, PACKAGE / SOURCE / 'go.mod',
                        *(PACKAGE / SOURCE).glob('*.go')]
        before = {path: hashlib.sha256(path.read_bytes()).hexdigest() for path in source_files}
        self.assertIn('module dockpipe.vm/guest-services/windows',
                      (PACKAGE / SOURCE / 'go.mod').read_text())

        with tempfile.TemporaryDirectory(prefix='dockpipe-agent-repro-') as temporary:
            root = Path(temporary)
            environment = {key: os.environ[key] for key in ('SYSTEMROOT', 'WINDIR') if key in os.environ}
            environment.update({
                'PATH': str(go.parent) + os.pathsep + os.defpath,
                'HOME': str(root / 'home'), 'TMPDIR': str(root),
                'TMP': str(root), 'TEMP': str(root),
                'GOPROXY': 'off', 'GOSUMDB': 'off', 'GOTOOLCHAIN': 'local',
                'GOWORK': 'off', 'GOMODCACHE': str(root / 'module-cache'),
                'GOCACHE': os.environ.get('GOCACHE', str(root / 'go-cache')),
                'GOMAXPROCS': '1', 'GOFLAGS': '-mod=readonly -p=1',
            })
            (root / 'home').mkdir()
            version = self.run_command([str(go), 'version'], environment, root, 10)
            self.assertEqual(version.split()[2], expected_toolchain)
            results = []
            for name in ('first-extraction', 'independent-second-extraction'):
                with self.subTest(extraction=name):
                    stage = root / name
                    source = stage / SOURCE
                    source.parent.mkdir(parents=True)
                    shutil.copytree(PACKAGE / SOURCE, source)
                    hook = stage / HOOK
                    hook.parent.mkdir(parents=True)
                    shutil.copyfile(PACKAGE / HOOK, hook)
                    runner = stage / 'build-service.py'
                    runner.write_text(
                        'import importlib.util, os, sys\n'
                        'from pathlib import Path\n'
                        'root = Path(__file__).resolve().parent\n'
                        'spec = importlib.util.spec_from_file_location("owning_vm_builder", root / "assets/scripts/build-source.py")\n'
                        'builder = importlib.util.module_from_spec(spec)\n'
                        'spec.loader.exec_module(builder)\n'
                        'builder.build_guest_services(root, root, dict(os.environ), sys.argv[1], root / "resolver")\n')
                    self.run_command([sys.executable, str(runner), str(go)], environment, stage)
                    binary = stage / 'resolver/assets/scripts/dockpipe-guest-agent.exe'
                    data = binary.read_bytes()
                    self.assertEqual(data[:2], b'MZ')
                    metadata = self.run_command([str(go), 'version', '-m', str(binary)],
                                                environment, stage, 10)
                    self.assertIn('\tpath\tdockpipe.vm/guest-services/windows\n', metadata)
                    self.assertIn('\tbuild\t-trimpath=true\n', metadata)
                    self.assertIn('\tbuild\tGOOS=windows\n', metadata)
                    metadata_lines = metadata.splitlines()[1:]
                    self.assertFalse(any(line.startswith('\tbuild\tvcs') for line in metadata_lines))
                    self.assertNotIn(str(root), '\n'.join(metadata_lines))
                    build_id = self.run_command([str(go), 'tool', 'buildid', str(binary)],
                                                environment, stage, 10)
                    results.append((hashlib.sha256(data).hexdigest(), metadata_lines, build_id))
                    for original in (PACKAGE / SOURCE).iterdir():
                        if original.is_file():
                            self.assertEqual((source / original.name).read_bytes(), original.read_bytes())
            self.assertEqual(results[0], results[1])

        for path, checksum in before.items():
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), checksum)


if __name__ == '__main__':
    unittest.main()
