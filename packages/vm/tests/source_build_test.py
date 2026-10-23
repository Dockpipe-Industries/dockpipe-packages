#!/usr/bin/env python3
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "assets/scripts/build-source.py"
specification = importlib.util.spec_from_file_location("vm_source_build", SCRIPT)
source_build = importlib.util.module_from_spec(specification)
specification.loader.exec_module(source_build)


class SourceBuildTests(unittest.TestCase):
    def test_missing_go_fails_before_workspace_or_cli_mutation(self):
        with mock.patch.object(source_build.shutil, "which", return_value=None), \
                mock.patch.object(source_build, "pinned_cli") as cli:
            with self.assertRaisesRegex(FileNotFoundError, "Go must be available"):
                source_build.main()
            cli.assert_not_called()

    def test_child_tool_uses_selected_path_and_missing_tool_fails_before_launch(self):
        with mock.patch.object(source_build.shutil, "which", return_value="/selected/go") as which, \
                mock.patch.object(source_build.subprocess, "run") as child:
            source_build.run(["go", "env"], Path("/source"), {"PATH": "/selected"})
        which.assert_called_once_with("go", path="/selected")
        self.assertEqual(child.call_args.args[0], ["/selected/go", "env"])
        with mock.patch.object(source_build.shutil, "which", return_value=None), \
                mock.patch.object(source_build.subprocess, "run") as child:
            with self.assertRaisesRegex(FileNotFoundError, "unavailable: go"):
                source_build.run(["go", "env"], Path("/source"), {"PATH": ""})
            child.assert_not_called()

    def test_windows_transient_staging_avoids_deep_scope_and_always_cleans_up(self):
        with tempfile.TemporaryDirectory() as temporary:
            scope = Path(temporary) / ("deep-package-scope-" * 6)
            scope.mkdir()
            for goos in ("windows", "linux"):
                with self.subTest(goos=goos):
                    created = None
                    with self.assertRaisesRegex(RuntimeError, "fixture failure"):
                        with source_build.temporary_staging(scope, goos) as directory:
                            created = Path(directory)
                            self.assertEqual(created.parent == scope, goos != "windows")
                            (created / "compiler-output").write_bytes(b"owned fixture")
                            raise RuntimeError("fixture failure")
                    self.assertIsNotNone(created)
                    self.assertFalse(created.exists())
                    self.assertTrue(scope.exists())

    def test_windows_launcher_uses_native_asset_and_preserves_operator_reference(self):
        with tempfile.TemporaryDirectory() as temporary:
            surface = Path(temporary) / "surface.json"
            surface.write_text(json.dumps({"actions": [{"id": "start-windows"}, {"id": "other"}]}))
            source_build.update_launcher(surface, "windows", "dockpipe-qemu-launch.exe")
            actions = json.loads(surface.read_text())["actions"]
            self.assertEqual(actions[0]["launch_environment"], {"windows": {
                "DOCKPIPE_VM_QEMU_REAL_BIN": {"inherit": "DOCKPIPE_VM_QEMU_BIN"},
                "DOCKPIPE_VM_QEMU_BIN": {"asset": "assets/tooling/bin/dockpipe-qemu-launch.exe"}}})
            self.assertEqual(actions[1], {"id": "other"})

    def test_factory_platform_or_library_mismatch_cannot_modify_surface(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            surface = root / "surface.json"
            factory = root / "factory.json"
            surface.write_text('{"actions": []}\n')
            original = surface.read_bytes()
            for document in ({"platform": "linux-amd64", "library": "libdockpipe-vm-display.so"},
                             {"platform": "windows-amd64", "library": "other.dll"}):
                factory.write_text(json.dumps(document))
                with self.assertRaises(ValueError):
                    source_build.update_factory(surface, factory, "windows", "amd64")
                self.assertEqual(surface.read_bytes(), original)

    def test_matching_factory_selects_only_its_native_platform(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            surface = root / "surface.json"
            factory = root / "factory.json"
            surface.write_text('{"actions": []}')
            factory.write_text(json.dumps({"platform": "darwin-arm64", "library": "libdockpipe-vm-display.dylib"}))
            source_build.update_factory(surface, factory, "darwin", "arm64")
            self.assertEqual(json.loads(surface.read_text())["native_display"], {
                "interface": 1, "libraries": {"macos": "assets/display/libdockpipe-vm-display.dylib"}})

    def test_manager_keeps_verified_offline_sdk_wrapper_and_failure_result(self):
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary)
            (repository / "VERSION").write_text("1.3.5\n")
            output = repository / "manager.exe"
            failure = subprocess.CalledProcessError(23, "catalog-go")
            with mock.patch.object(source_build, "run", side_effect=failure) as run, \
                    mock.patch.object(source_build, "emit_result") as emit:
                with self.assertRaises(subprocess.CalledProcessError):
                    source_build.build_manager(repository, {}, Path("/pinned/dockpipe"), "manager.exe", output)
            command = run.call_args.args[0]
            self.assertEqual(command[1], repository / "tooling/catalog_go.py")
            self.assertEqual(command[2:6], ["--", "build", "-buildvcs=false", "-trimpath"])
            self.assertIn("-s -w -X main.Version=1.3.5", command)
            self.assertEqual([call.args[3] for call in emit.call_args_list], ["start", "fail"])

    def test_explicit_empty_sdk_or_partial_guest_bundle_fails_without_compilation(self):
        cases = [
            {"DOCKPIPE_VM_DISPLAY_SDK_PREFIX": "", "DOCKPIPE_VM_HOST_QT_PREFIX": "/qt"},
            {"DOCKPIPE_VM_GUEST_TOOLS_ARCHIVE": "/bundle.zip"},
        ]
        for extra in cases:
            with self.subTest(extra=extra), tempfile.TemporaryDirectory() as temporary:
                repository = Path(temporary)
                package = repository / "packages/vm"
                assets = package / "resolvers/qemu/assets/ui"
                assets.mkdir(parents=True)
                (assets / "surface.json").write_text('{"actions": [{"id": "start-windows"}]}')
                (package / "resolvers/qemu/profile").write_text("")
                scope = repository / "scope"
                scope.mkdir()
                environment = {"DOCKPIPE_PACKAGE_ROOT": str(package), "DOCKPIPE_BIN": "/pinned/dockpipe", **extra}
                commands = []

                def run(arguments, root, child_environment, capture=False):
                    commands.append(arguments)
                    value = json.dumps({"GOEXE": ".exe", "GOOS": "windows", "GOARCH": "amd64"}) \
                        if arguments[1] == "env" else str(scope)
                    return subprocess.CompletedProcess(arguments, 0, stdout=value)

                def build(root, child_environment, binary, tool, output):
                    output.write_bytes(b"owned test executable")

                with mock.patch.dict(os.environ, environment, clear=True), \
                        mock.patch.object(source_build, "pinned_cli", return_value=Path("/pinned/dockpipe")), \
                        mock.patch.object(source_build.shutil, "which", return_value="/selected/go"), \
                        mock.patch.object(source_build, "run", side_effect=run), \
                        mock.patch.object(source_build, "build_manager", side_effect=build):
                    with self.assertRaises(ValueError):
                        source_build.main()
                self.assertEqual(len(commands), 2)
                self.assertEqual(list(scope.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
