"""VM runtime floors survive source compilation and independent installation."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

import yaml


PACKAGE = Path(__file__).resolve().parents[1]
MINIMUM = "0.6.28-0"
VERSION = "1.3.7"
OLD_CLI = os.environ.get("DOCKPIPE_VM_COMPAT_OLD_BIN", "")
OLD_VERSION = os.environ.get("DOCKPIPE_VM_COMPAT_OLD_VERSION", "0.6.27")
NEW_CLI = os.environ.get("DOCKPIPE_VM_COMPAT_NEW_BIN", "")
NEW_VERSION = os.environ.get("DOCKPIPE_VM_COMPAT_NEW_VERSION", "")


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

    def test_boot_apps_install_the_resolver_and_preserve_actual_host_contract(self):
        expected = {"linux-vm": ["deb"], "windows-vm": ["windows", "deb", "macos"]}
        for name, platforms in expected.items():
            with self.subTest(workflow=name):
                workflow = PACKAGE / "workflows" / name
                manifest = yaml.safe_load((workflow / "package.yml").read_text())
                config = yaml.safe_load((workflow / "config.yml").read_text())
                self.assertEqual(manifest["requires_resolvers"], ["qemu"])
                self.assertEqual(manifest["platforms"], platforms)
                self.assertEqual(config["platforms"], platforms)
                self.assertEqual(config["runtime"], "vm")
                self.assertEqual(config["resolver"], "qemu")

    def test_family_version_does_not_relabel_legacy_controller(self):
        manifest = yaml.safe_load((PACKAGE / "package.yml").read_text())
        self.assertEqual(manifest["version"], VERSION)
        self.assertEqual(manifest["min_dockpipe_version"], MINIMUM)
        controller = (PACKAGE / "tools/cmd/dockpipe-qemu-controller/main.go").read_text()
        self.assertIn('const version = "1.3.5"', controller)

    def test_host_bash_picker_is_part_of_the_actual_advanced_page(self):
        config = yaml.safe_load((PACKAGE / "workflows/windows-vm/config.yml").read_text())
        page = next(item for item in config["view"]["pages"] if item["id"] == "advanced")
        section = next(item for item in page["sections"] if item["id"] == "host-tools")
        self.assertEqual(section["fields"], ["Advanced.HostBashPath"])
        self.assertNotIn("Advanced.HostBashPath", config.get("inputs", {}))

    def test_pinned_cli_compiles_picker_and_propagates_host_bash_input(self):
        selected_cli = os.environ.get("DOCKPIPE_BIN", "")
        binary = Path(selected_cli)
        self.assertTrue(selected_cli and binary.is_absolute() and binary.is_file(),
                        "select the actual pinned DOCKPIPE_BIN for the compiler/consumer test")
        with tempfile.TemporaryDirectory(prefix="vm-host-bash-input-") as temporary:
            root = Path(temporary)
            workflow = root / "workflows/host-bash"
            workflow.mkdir(parents=True)
            shutil.copytree(PACKAGE / "resolvers/qemu/models", workflow / "models")
            selected_bash = str(root / "selected Bash executable")
            config = {
                "name": "host-bash-field-consumer", "docker_preflight": False,
                "types": ["models/QemuVmResolverConfig"],
                "inputs": {"Advanced.HostBashPath": selected_bash},
                "steps": [{"id": "capture-input", "kind": "host", "run": "capture.py"}],
            }
            (workflow / "config.yml").write_text(yaml.safe_dump(config, sort_keys=False))
            # This Python step observes the typed environment only. It never
            # invokes the selected path; executable admission belongs to core.
            (workflow / "capture.py").write_text(
                "import json, os\nfrom pathlib import Path\n"
                "Path('captured.json').write_text(json.dumps({"
                "'selected': os.environ.get('DOCKPIPE_HOST_BASH_BIN')}))\n")
            environment = {name: value for name, value in os.environ.items()
                           if not name.startswith("DOCKPIPE_")}
            for name in ("BASH_ENV", "ENV", "PYTHONPATH", "PYTHONHOME"):
                environment.pop(name, None)
            environment.update(DOCKPIPE_BIN=str(binary), DOCKPIPE_HOST_PYTHON_BIN=sys.executable,
                               DOCKPIPE_GLOBAL_ROOT=str(root / "global"),
                               DOCKPIPE_SYSTEM_ROOT=str(root / "system"))
            for kind in ("CACHE", "STATE", "CONFIG", "DATA"):
                environment["XDG_" + kind + "_HOME"] = str(root / kind.lower())

            def invoke(*arguments):
                result = subprocess.run([str(binary), *map(str, arguments)], cwd=root,
                                        env=environment, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return result.stdout

            invoke("pipelang", "compile", "--in", workflow / "models/WindowsVmAdvanced.pipe",
                   "--entry", "WindowsVmAdvanced", "--out", root / "compiled")
            compiled = json.loads((root / "compiled/WindowsVmAdvanced.bindings.json").read_text())
            field = next(item for item in compiled["fields"] if item["name"] == "HostBashPath")
            self.assertEqual(field["value"], "")
            catalog = json.loads(invoke("catalog", "list", "--format", "json", "--workdir", root))

            def find_field(value):
                if isinstance(value, dict):
                    if value.get("env_name") == "DOCKPIPE_HOST_BASH_BIN":
                        return value
                    values = value.values()
                elif isinstance(value, list):
                    values = value
                else:
                    return None
                for child in values:
                    found = find_field(child)
                    if found is not None:
                        return found
                return None

            described = find_field(catalog)
            self.assertIsNotNone(described)
            self.assertEqual(described["field_name"], "HostBashPath")
            self.assertEqual(described["attributes"]["control"], "file")
            self.assertEqual(described["attributes"]["filemode"], "open-file")
            self.assertIn("Absolute path", described["attributes"]["placeholder"])
            invoke("--workflow-file", workflow / "config.yml", "--workdir", root,
                   "--no-vault", "--no-op-inject", "--")
            self.assertEqual(json.loads((root / "captured.json").read_text()),
                             {"selected": selected_bash})

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

            self.assertEqual(OLD_VERSION.split("-", 1)[0].split("+", 1)[0], "0.6.27",
                             "select the actual previous public 0.6.27 runtime")
            self.assertEqual(invoke(OLD_CLI, "--version").stdout.strip(), OLD_VERSION)
            self.assertTrue(NEW_VERSION, "select the exact published backend-capable CLI version")
            self.assertEqual(invoke(NEW_CLI, "--version").stdout.strip(), NEW_VERSION)
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
