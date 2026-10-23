#!/usr/bin/env python3
"""Build the owning VM tools and installed QEMU resolver without a shell dependency."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


PLATFORMS = {"linux": "linux", "windows": "windows", "darwin": "macos"}
LIBRARIES = {
    "linux": "libdockpipe-vm-display.so",
    "windows": "dockpipe-vm-display.dll",
    "darwin": "libdockpipe-vm-display.dylib",
}


def run(arguments, repository, environment, capture=False):
    command = [str(argument) for argument in arguments]
    if not Path(command[0]).is_absolute():
        executable = shutil.which(command[0], path=environment.get("PATH", os.defpath))
        if not executable:
            raise FileNotFoundError("Native source-build tool is unavailable: " + command[0])
        command[0] = executable
    return subprocess.run(command, cwd=repository, env=environment, check=True,
                          text=True, encoding="utf-8", stdout=subprocess.PIPE if capture else None)



def pinned_cli(environment):
    value = environment.get("DOCKPIPE_BIN", "")
    binary = Path(value)
    if not value or not binary.is_absolute() or not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError("DOCKPIPE_BIN must select an absolute pinned executable")
    return binary


def emit_result(binary, repository, environment, status, fields, duration=None, error=None):
    arguments = [str(binary), "result", "--unit", "package.source.tool", "--status", status]
    if duration is not None:
        arguments += ["--duration-ms", str(duration)]
    for name, value in fields.items():
        arguments += ["--id", name + "=" + str(value)]
    if error:
        arguments += ["--error", error]
    try:
        result = subprocess.run(arguments, cwd=repository, env=environment, timeout=10, check=False)
        if result.returncode == 0:
            return
    except (OSError, subprocess.TimeoutExpired):
        pass
    print("[dockpipe] unit=package.source.tool status=" + status, file=sys.stderr)


def build_manager(repository, environment, binary, tool, output):
    version_path = repository / "VERSION"
    version = "".join(version_path.read_text().split()) if version_path.is_file() else "0.0.0"
    fields = {"tool": tool, "output": output, "package": "vm"}
    emit_result(binary, repository, environment, "start", fields)
    started = time.monotonic()
    try:
        run([sys.executable, repository / "tooling/catalog_go.py", "--", "build",
             "-buildvcs=false", "-trimpath", "-ldflags", "-s -w -X main.Version=" + version,
             "-o", output, "./packages/vm/management/cmd/dockpipe-vm-manager"], repository, environment)
    except (OSError, subprocess.CalledProcessError):
        duration = int((time.monotonic() - started) * 1000)
        emit_result(binary, repository, environment, "fail", fields, duration, "catalog Go build failed")
        raise
    emit_result(binary, repository, environment, "done", fields,
                int((time.monotonic() - started) * 1000))


def update_launcher(surface_path, goos, launcher):
    platform_key = PLATFORMS.get(goos)
    if platform_key is None or Path(launcher).name != launcher:
        raise ValueError("Unsupported native QEMU launcher platform or asset")
    surface = json.loads(surface_path.read_text())
    for action in surface.get("actions", []):
        if action.get("id") == "start-windows":
            action["launch_environment"] = {
                platform_key: {
                    "DOCKPIPE_VM_QEMU_REAL_BIN": {"inherit": "DOCKPIPE_VM_QEMU_BIN"},
                    "DOCKPIPE_VM_QEMU_BIN": {"asset": "assets/tooling/bin/" + launcher},
                }
            }
    surface_path.write_text(json.dumps(surface, indent=2) + "\n")


def update_factory(surface_path, factory_path, goos, goarch):
    factory = json.loads(factory_path.read_text())
    library = LIBRARIES.get(goos)
    platform_key = PLATFORMS.get(goos)
    if (goarch not in {"amd64", "arm64"} or platform_key is None
            or factory.get("platform") != goos + "-" + goarch or factory.get("library") != library):
        raise ValueError("Unsupported native display factory build output")
    surface = json.loads(surface_path.read_text())
    surface["native_display"] = {"interface": 1, "libraries": {platform_key: "assets/display/" + library}}
    surface_path.write_text(json.dumps(surface, indent=2) + "\n")


def temporary_staging(staging_root, goos):
    # MSBuild cannot compile inside a deeply nested durable package scope even
    # when the source files themselves fit Windows' legacy path limit. Only
    # transient source/compiler files use the short native temporary directory.
    directory = None if goos == "windows" else staging_root
    return tempfile.TemporaryDirectory(prefix="qemu.", dir=directory)


def main():
    environment = dict(os.environ)
    go_executable = shutil.which("go", path=environment.get("PATH", os.defpath))
    if not go_executable:
        raise FileNotFoundError("Go must be available in the selected native tool PATH")
    configured = environment.get("DOCKPIPE_PACKAGE_ROOT")
    package = Path(configured).resolve() if configured else Path(__file__).resolve().parents[2]
    repository = package.parents[1]
    binary = pinned_cli(environment)
    legacy_library = environment.get("DOCKPIPE_SOURCE_BUILD_LIB")
    if legacy_library and not Path(legacy_library).samefile(repository / "tooling/lib/package-source-build.sh"):
        raise ValueError("Custom Bash source-build libraries are not native Python build inputs")
    output = repository / "bin/.dockpipe/tooling/bin"
    build = repository / "bin/.dockpipe/build"
    output.mkdir(parents=True, exist_ok=True)
    for directory in (build / "go-cache", build / "go-tmp"):
        directory.mkdir(parents=True, exist_ok=True)
    environment["GOCACHE"] = environment.get("GOCACHE") or str(build / "go-cache")
    environment["GOTMPDIR"] = environment.get("GOTMPDIR") or str(build / "go-tmp")
    go = run([go_executable, "env", "-json", "GOEXE", "GOOS", "GOARCH"], repository, environment, capture=True)
    if len(go.stdout) > 65536:
        raise ValueError("Go platform metadata exceeded its bound")
    platform = json.loads(go.stdout)
    goos, goarch = platform["GOOS"], platform["GOARCH"]
    suffix = platform["GOEXE"]
    if goos not in PLATFORMS or suffix != (".exe" if goos == "windows" else ""):
        raise ValueError("Unsupported native Go tool platform")
    tool = "dockpipe-vm-manager" + suffix
    build_manager(repository, environment, binary, tool, output / tool)
    scope_environment = dict(environment, DOCKPIPE_PACKAGE_MANIFEST=str(package / "package.yml"))
    scope = run([binary, "scope", "--package", "vm.build.v1", "resolver-build", "--workdir", repository],
                repository, scope_environment, capture=True).stdout.strip()
    if len(scope) > 4096 or any(character in scope for character in "\x00\r\n") or not Path(scope).is_absolute():
        raise ValueError("Package SDK did not return an absolute bounded build scope")
    staging_root = Path(scope)
    staging_root.mkdir(parents=True, exist_ok=True)
    with temporary_staging(staging_root, goos) as temporary:
        staging = Path(temporary)
        resolver = staging / "resolvers/qemu"
        shutil.copytree(package / "resolvers/qemu", resolver, symlinks=True)
        tooling = resolver / "assets/tooling"
        (tooling / "bin").mkdir(parents=True, exist_ok=True)
        shutil.copy2(output / tool, tooling / "bin" / tool)
        launcher = "dockpipe-qemu-launch" + suffix
        shutil.copy2(output / tool, tooling / "bin" / launcher)
        surface = resolver / "assets/ui/surface.json"
        update_launcher(surface, goos, launcher)
        qt = environment.get("DOCKPIPE_VM_HOST_QT_PREFIX", "")
        if qt and goos == "linux":
            run([sys.executable, package / "clipboard/build.py", "--output", tooling], repository, environment)
        if "DOCKPIPE_VM_DISPLAY_SDK_PREFIX" in environment:
            if not environment["DOCKPIPE_VM_DISPLAY_SDK_PREFIX"] or not qt:
                raise ValueError("The native display build requires explicit nonempty SDK and host Qt inputs")
            display = staging / "display-build"
            run([sys.executable, package / "display/build.py", "--output", display], repository, environment)
            closure = display / "package/assets/display"
            shutil.copytree(closure, resolver / "assets/display")
            update_factory(surface, closure / "display-factory.json", goos, goarch)
        archive_present = "DOCKPIPE_VM_GUEST_TOOLS_ARCHIVE" in environment
        descriptor_present = "DOCKPIPE_VM_GUEST_TOOLS_DESCRIPTOR" in environment
        if archive_present or descriptor_present:
            archive = environment.get("DOCKPIPE_VM_GUEST_TOOLS_ARCHIVE", "")
            descriptor = environment.get("DOCKPIPE_VM_GUEST_TOOLS_DESCRIPTOR", "")
            if not archive or not descriptor:
                raise ValueError("Guest Tools archive and descriptor must both be explicit nonempty inputs")
            run([sys.executable, package / "guest-tools/tooling/materialize_bundle.py",
                 "--archive", archive, "--descriptor", descriptor,
                 "--destination", resolver / "assets/guest-tools/windows"], repository, environment)
            with (resolver / "profile").open("a") as profile:
                profile.write("DOCKPIPE_RESOLVER_VM_GUEST_TOOLS_BUNDLE=assets/guest-tools/windows\n")
        run([binary, "package", "compile", "resolvers", "--workdir", repository,
             "--from", staging / "resolvers", "--force"], repository, environment)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print("VM source build failed: " + str(error), file=sys.stderr)
        raise SystemExit(1)
