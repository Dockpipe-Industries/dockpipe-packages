"""VM runtime floors survive source compilation and independent installation."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

import yaml


PACKAGE = Path(__file__).resolve().parents[1]
MINIMUM = "0.6.26-0"
VERSION = "1.3.6"
OLD_CLI = os.environ.get("DOCKPIPE_VM_COMPAT_OLD_BIN", "")
NEW_CLI = os.environ.get("DOCKPIPE_VM_COMPAT_NEW_BIN", "")


class RuntimeCompatibilityTests(unittest.TestCase):
    def test_every_installable_slice_has_its_own_runtime_floor(self):
        configs = sorted((PACKAGE / "workflows").glob("*/config.yml"))
        self.assertEqual(len(configs), 4)
        manifests = [PACKAGE / "resolvers/qemu/package.yml"]
        manifests.extend(config.parent / "package.yml" for config in configs)
        for path in manifests:
            with self.subTest(manifest=str(path.relative_to(PACKAGE))):
                manifest = yaml.safe_load(path.read_text())
                self.assertIn(manifest["kind"], ("workflow", "resolver"))
                self.assertEqual(manifest["version"], VERSION)
                self.assertEqual(manifest["min_dockpipe_version"], MINIMUM)
                self.assertEqual(manifest["name"], path.parent.name)

    def test_family_version_does_not_relabel_legacy_controller(self):
        manifest = yaml.safe_load((PACKAGE / "package.yml").read_text())
        self.assertEqual(manifest["version"], VERSION)
        self.assertEqual(manifest["min_dockpipe_version"], MINIMUM)
        controller = (PACKAGE / "tools/cmd/dockpipe-qemu-controller/main.go").read_text()
        self.assertIn('const version = "1.3.5"', controller)

    @unittest.skipUnless(OLD_CLI and NEW_CLI, "select explicit old/new CLI binaries for admission proof")
    def test_real_cli_source_installed_metadata_and_safe_resolver_asset_admission(self):
        for binary in (OLD_CLI, NEW_CLI):
            self.assertTrue(Path(binary).is_absolute())
            self.assertTrue(Path(binary).is_file())
        with tempfile.TemporaryDirectory(prefix="dockpipe-vm-compat-") as temporary:
            root = Path(temporary)
            vm = root / "packages/vm"
            shutil.copytree(PACKAGE, vm, ignore=shutil.ignore_patterns("__pycache__"))
            (root / "dockpipe.config.json").write_text(json.dumps({
                "schema": 1, "compile": {"workflows": ["packages"]},
                "packages": {"namespace": "dockpipeproject"}}))
            environment = dict(os.environ)
            for key in ("DOCKPIPE_REPO_ROOT", "DOCKPIPE_PACKAGES_ROOT", "DOCKPIPE_GLOBAL_ROOT",
                        "DOCKPIPE_WORKSPACE_PACKAGE", "DOCKPIPE_WORKDIR"):
                environment.pop(key, None)
            for kind in ("CACHE", "STATE", "CONFIG", "DATA"):
                environment["XDG_" + kind + "_HOME"] = str(root / kind.lower())

            def invoke(binary, *arguments):
                return subprocess.run([binary, *map(str, arguments)], cwd=root, env=environment,
                                      capture_output=True, text=True, timeout=30)

            self.assertEqual(invoke(OLD_CLI, "--version").stdout.strip(), "0.6.25")
            self.assertTrue(invoke(NEW_CLI, "--version").stdout.strip().startswith("0.6.26-staging."))
            for kind in ("workflows", "resolvers"):
                result = invoke(NEW_CLI, "package", "compile", kind, "--workdir", root,
                                "--from", vm / kind)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            store = root / "exported"
            result = invoke(NEW_CLI, "package", "build", "store", "--workdir", root, "--out", store)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            exported = json.loads((store / "packages-store-manifest.json").read_text())
            entries = exported["packages"]["resolvers"] + exported["packages"]["workflows"]
            self.assertEqual(len(entries), 5)
            for entry in entries:
                self.assertEqual(entry["version"], VERSION)
                self.assertEqual(entry["min_dockpipe_version"], MINIMUM)
            installed = root / "installed"
            installed.mkdir()
            for archive in store.glob("*.tar.gz"):
                with tarfile.open(archive) as bundle:
                    for member in bundle.getmembers():
                        destination = installed / member.name
                        self.assertTrue(destination.resolve().is_relative_to(installed))
                        if member.isdir():
                            destination.mkdir(parents=True, exist_ok=True)
                        else:
                            self.assertTrue(member.isfile())
                            self.assertLessEqual(member.size, 8 << 20)
                            destination.parent.mkdir(parents=True, exist_ok=True)
                            destination.write_bytes(bundle.extractfile(member).read())
            for source in (vm / "workflows", installed / "workflows"):
                for config in sorted(source.glob("*/config.yml")):
                    with self.subTest(config=str(config.relative_to(root))):
                        rejected = invoke(OLD_CLI, "workflow", "validate", config)
                        self.assertNotEqual(rejected.returncode, 0)
                        self.assertIn("requires Dockpipe " + MINIMUM, rejected.stderr)
                        accepted = invoke(NEW_CLI, "workflow", "validate", config)
                        self.assertEqual(accepted.returncode, 0, accepted.stdout + accepted.stderr)
            # The public asset query loads the actual resolver metadata without
            # executing a guest workflow, manager, QEMU, or private broker.
            rejected = invoke(OLD_CLI, "scope", "resolver", "qemu", "assets-dir", "--workdir", root)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn('package "qemu" requires Dockpipe ' + MINIMUM, rejected.stderr)
            accepted = invoke(NEW_CLI, "scope", "resolver", "qemu", "assets-dir", "--workdir", root)
            self.assertEqual(accepted.returncode, 0, accepted.stdout + accepted.stderr)
            self.assertTrue(Path(accepted.stdout.strip()).is_dir())


if __name__ == "__main__":
    unittest.main()
