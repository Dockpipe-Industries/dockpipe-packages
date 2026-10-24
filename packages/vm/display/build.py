#!/usr/bin/env python3
"""Build a native display adapter against explicit installed SDK and Qt artifacts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys

# Producer imports must not create generated files in the authored package tree.
sys.dont_write_bytecode = True
from tooling.runtime_closure import windows_closure, macos_closure
from tooling.broker_closure import broker_inputs, bundle_broker
from tooling.windows_test_diagnostics import diagnose_tests

ROOT = Path(__file__).resolve().parent
SYSTEM_ABI = {"libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1"}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def selected_directory(name):
    value = os.environ.get(name)
    if not value or not Path(value).is_absolute() or not Path(value).is_dir():
        raise ValueError(name + " must name an explicit absolute installed directory")
    return Path(value).resolve()


def run(arguments, **options):
    return subprocess.run([str(item) for item in arguments], check=True, timeout=300, **options)



def bounded_text(path, limit):
    if not path.is_file() or path.is_symlink() or path.stat().st_size > limit:
        raise ValueError("Missing or out-of-bounds prepared build input: " + path.name)
    return path.read_text()


def same_directory(value, expected):
    if not value or not Path(value).is_absolute():
        return False
    try:
        return Path(value).samefile(expected)
    except OSError:
        return False


def material_sources(root):
    paths = [root / "CMakeLists.txt"]
    for directory in ("src", "tests", "tooling"):
        paths.extend(path for path in (root / directory).rglob("*")
                     if path.is_file() and path.suffix in {".cpp", ".h", ".in", ".cmake"})
    if len(paths) > 256:
        raise ValueError("Prepared build source inventory exceeds 256 files")
    inventory = {}
    total = 0
    for path in paths:
        if path.is_symlink() or not path.is_file() or path.stat().st_size > 1024 * 1024:
            raise ValueError("Prepared build material source is missing or unbounded")
        total += path.stat().st_size
        inventory[path.relative_to(root).as_posix()] = sha256(path)
    if total > 10 * 1024 * 1024:
        raise ValueError("Prepared build material sources exceed 10 MiB")
    return inventory


def admit_prepared_build(build, sdk_config, qt_config, sdk_name, selector, system, cpu, gio=None):
    if not build.is_absolute() or not build.is_dir() or build.is_symlink():
        raise ValueError("Prepared build must select an explicit absolute directory")
    build = build.resolve()
    cache = {}
    for line in bounded_text(build / "CMakeCache.txt", 1024 * 1024).splitlines():
        match = re.fullmatch(r"([^#/:][^:]*):[^=]+=(.*)", line)
        if match:
            if match[1] in cache:
                raise ValueError("Duplicate prepared build cache field")
            cache[match[1]] = match[2]
    source_value = cache.get("CMAKE_HOME_DIRECTORY", "")
    source = Path(source_value)
    if not source_value or not source.is_absolute() or not source.is_dir() or source.is_symlink():
        raise ValueError("Prepared build source directory is unavailable")
    if material_sources(source.resolve()) != material_sources(ROOT):
        raise ValueError("Prepared build material source differs from this producer")
    for name, expected in ((sdk_name + "_DIR", sdk_config), ("Qt6_DIR", qt_config)):
        value = cache.get(name, "")
        if not same_directory(value, expected):
            raise ValueError("Prepared build selected a different " + name)
    if cache.get("CMAKE_BUILD_TYPE") != "Release" or cache.get("DOCKPIPE_BUILD_DISPLAY_PLUGIN") != "ON" \
            or cache.get("DOCKPIPE_DISPLAY_SDK") != selector:
        raise ValueError("Prepared build configuration differs from the selected release adapter")
    if gio is not None:
        value = cache.get("DOCKPIPE_GIO_PREFIX", "")
        if not same_directory(value, gio):
            raise ValueError("Prepared build selected a different native dependency prefix")
    configuration = bounded_text(sdk_config / (sdk_name + "Config.cmake"), 65536)
    fields = {}
    for field in ("COMPILER_ID", "COMPILER_VERSION", "SYSTEM_NAME", "SYSTEM_PROCESSOR"):
        matches = re.findall(r'set\(' + re.escape(sdk_name + "_" + field) + r' "([^"\n]+)"\)', configuration)
        if len(matches) != 1:
            raise ValueError("Selected installed SDK compatibility field is missing or ambiguous")
        fields[field] = matches[0]
    versions = re.findall(r"find_dependency\(Qt6 ([0-9.]+) EXACT", configuration)
    if len(versions) != 1 or fields["SYSTEM_NAME"] != system:
        raise ValueError("Selected installed SDK has a different platform or Qt contract")
    sdk_cpu = fields["SYSTEM_PROCESSOR"].lower()
    if (cpu == "amd64" and sdk_cpu not in {"amd64", "x86_64"}) \
            or (cpu == "arm64" and sdk_cpu not in {"arm64", "aarch64"}):
        raise ValueError("Selected installed SDK has a different processor contract")
    metadata = json.loads(bounded_text(build / "display-metadata.json", 16384))
    expected = {"display_abi": 1, "qt_version": versions[0],
                "build_abi": ("x86_64" if cpu == "amd64" else "arm64")
                    + "-little_endian-" + ("llp64" if system == "Windows" else "lp64"),
                "compiler_id": fields["COMPILER_ID"], "compiler_version": fields["COMPILER_VERSION"]}
    if metadata != expected:
        raise ValueError("Prepared build metadata differs from the exact installed SDK contract")
    return build


def elf_dependencies(binary):
    output = subprocess.check_output(["ldd", str(binary)], text=True)
    if "not found" in output:
        raise ValueError("Native runtime closure has unresolved libraries")
    libraries = {}
    for line in output.splitlines():
        match = re.match(r"\s*(\S+) => (/\S+) \(", line)
        if match and match[1] not in SYSTEM_ABI:
            libraries[match[1]] = Path(match[2]).resolve()
    if len(libraries) > 32:
        raise ValueError("Native runtime closure exceeds declared library bound")
    return libraries


def gio_closure():
    from tooling.linux_prefix import selected
    prefix = selected()
    if prefix is not None:
        gio = next((path for path in prefix.libraries if path.name == "libgio-2.0.so.0"), None)
        if gio is None:
            raise ValueError("Declared Linux prefix lacks GIO")
        libraries = elf_dependencies(gio)
        libraries[gio.name] = gio
        return prefix.closure(libraries)
    library_directory = Path(subprocess.check_output(
        ["pkg-config", "--variable=libdir", "gio-2.0"], text=True).strip())
    gio = library_directory / "libgio-2.0.so.0"
    libraries = elf_dependencies(gio)
    libraries[gio.name] = gio.resolve()
    return libraries


def owning_package(path):
    # Ubuntu's merged-/usr aliases differ in dpkg's file database.
    candidates = [path]
    if str(path).startswith("/lib/"):
        candidates.append(Path("/usr") / path.relative_to("/"))
    elif str(path).startswith("/usr/lib/"):
        candidates.append(Path("/") / path.relative_to("/usr"))
    for candidate in candidates:
        result = subprocess.run(["dpkg-query", "-S", str(candidate)],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        if result.returncode == 0:
            return result.stdout.split(": ", 1)[0]
    raise ValueError("No package license provenance for " + path.name)


def package_license(path):
    from tooling.linux_prefix import selected
    prefix = selected()
    if prefix is not None:
        return prefix.package_license(path)
    package = owning_package(path)
    source = Path("/usr/share/doc") / package.split(":", 1)[0] / "copyright"
    if not source.is_file():
        raise ValueError("Runtime dependency copyright is missing: " + package)
    version = subprocess.check_output(["dpkg-query", "-W", "-f=${Version}", package], text=True).strip()
    return package, version, source


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--prepared-build", type=Path,
                        help="Package an explicit source/SDK/Qt-verified existing release build")
    parser.add_argument("--skip-tests", action="store_true",
                        help="Build only; caller must run socket lifecycle tests through reviewed host entrypoint")
    args = parser.parse_args()
    system = platform.system()
    machine = platform.machine().lower()
    if system not in {"Linux", "Windows", "Darwin"} or machine not in {"x86_64", "amd64", "arm64", "aarch64"}:
        raise ValueError("Display producer requires a supported native 64-bit host")
    cpu = "arm64" if machine in {"arm64", "aarch64"} else "amd64"
    platform_name = {"Linux": "linux", "Windows": "windows", "Darwin": "darwin"}[system] + "-" + cpu
    broker_binary, broker_provenance = broker_inputs()
    sdk = selected_directory("DOCKPIPE_VM_DISPLAY_SDK_PREFIX")
    qt = selected_directory("DOCKPIPE_VM_HOST_QT_PREFIX")
    if not args.output.is_absolute() or args.output.exists():
        raise ValueError("Output must select a fresh absolute task artifact directory")
    args.output.mkdir(parents=True)
    build = args.output / "build"
    selector = os.environ.get("DOCKPIPE_DISPLAY_SDK", "launcher")
    if selector not in {"launcher", "display"}:
        raise ValueError("DOCKPIPE_DISPLAY_SDK must explicitly select launcher or display")
    sdk_name = "DockPipeLauncherSDK" if selector == "launcher" else "DockPipeDisplaySDK"
    sdk_config = sdk / "lib/cmake" / sdk_name
    if not (sdk_config / (sdk_name + "Config.cmake")).is_file():
        raise ValueError("Selected installed SDK configuration is missing")
    qt_config = qt / "lib/cmake/Qt6"
    if not (qt_config / "Qt6Config.cmake").is_file():
        raise ValueError("Selected exact Qt kit configuration is missing")
    arguments = ["cmake", "-S", ROOT, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
        "-D" + sdk_name + "_DIR=" + str(sdk_config), "-DQt6_DIR=" + str(qt_config),
        "-DDOCKPIPE_BUILD_DISPLAY_PLUGIN=ON", "-DDOCKPIPE_DISPLAY_SDK=" + selector,
        "-DCMAKE_PREFIX_PATH=" + str(sdk) + ";" + str(qt)]
    gio = None
    brew = None
    inspector = None
    if system == "Windows":
        gio = selected_directory("DOCKPIPE_GIO_PREFIX")
        arguments.append("-DDOCKPIPE_GIO_PREFIX=" + str(gio))
        os.environ["PATH"] = str(gio / "bin") + os.pathsep + str(qt / "bin") + os.pathsep + os.environ.get("PATH", "")
    if args.prepared_build is not None:
        build = admit_prepared_build(args.prepared_build, sdk_config, qt_config,
                                     sdk_name, selector, system, cpu, gio)
    else:
        run(arguments)
        run(["cmake", "--build", build, "--config", "Release", "-j4"])
    if not args.skip_tests:
        try:
            run(["ctest", "--test-dir", build, "-C", "Release", "--output-on-failure"])
        except subprocess.CalledProcessError:
            if system == "Windows":
                try:
                    diagnose_tests(build, gio, qt, os.environ)
                except (OSError, ValueError, subprocess.SubprocessError) as diagnostic_error:
                    print("Windows test evidence collection failed: " + str(diagnostic_error)[:2000], file=sys.stderr)
            raise
    destination = args.output / "package/assets/display"
    destination.mkdir(parents=True)
    module_name = {"Linux": "libdockpipe-vm-display.so", "Windows": "dockpipe-vm-display.dll",
                   "Darwin": "libdockpipe-vm-display.dylib"}[system]
    module = destination / module_name
    shutil.copyfile(build / "package/assets/display" / module.name, module)
    provenance = []
    if system == "Windows":
        inspector_value = os.environ.get("DOCKPIPE_VM_DISPLAY_INSPECTOR")
        if not inspector_value or not Path(inspector_value).is_absolute():
            raise ValueError("DOCKPIPE_VM_DISPLAY_INSPECTOR must select native objdump or dumpbin")
        inspector = Path(inspector_value)
        provenance = windows_closure(module, destination, gio, qt, inspector)
    elif system == "Darwin":
        gio = Path(subprocess.check_output(["pkg-config", "--variable=prefix", "gio-2.0"], text=True).strip())
        brew = selected_directory("DOCKPIPE_VM_DISPLAY_BREW_PREFIX")
        provenance = macos_closure(module, destination, gio, brew)
    else:
        libraries = gio_closure()
        runtime = destination / "lib"
        runtime.mkdir()
        licenses = destination / "licenses"
        licenses.mkdir()
        provenance = []
        for name, source in sorted(libraries.items()):
            shutil.copyfile(source, runtime / name)
            package, version, license_source = package_license(source)
            shutil.copyfile(license_source, licenses / (package.replace(":", "-") + ".copyright"))
            provenance.append({"name": name, "sha256": sha256(runtime / name),
                               "package": package, "version": version.strip()})
    bundle_broker(broker_binary, broker_provenance, destination, system, gio, qt,
                  inspector, brew, elf_dependencies, package_license)
    metadata = json.loads((build / "display-metadata.json").read_text())
    descriptor = {"schema": 1, "interface": "com.dockpipe.launcher.DisplayPackage/1.0",
                  "platform": platform_name, "library": module.name,
                  "sha256": sha256(module), "size_bytes": module.stat().st_size, **metadata}
    (destination / "display-factory.json").write_text(json.dumps(descriptor, indent=2) + "\n")
    (destination / "runtime-dependencies.json").write_text(json.dumps({
        "schema": 1, "libraries": provenance,
        "host_abi": platform_name + "; exact host Qt/build/compiler ABI required",
        "qt_runtime": "provided by matching host; never bundled by this module",
        "tests_run": not args.skip_tests,
        "prepared_build": args.prepared_build is not None}, indent=2) + "\n")
    source_inventory = []
    for path in sorted(ROOT.rglob("*")):
        if path.is_file() and "__pycache__" not in path.parts:
            source_inventory.append({"path": path.relative_to(ROOT).as_posix(),
                                     "size_bytes": path.stat().st_size, "sha256": sha256(path)})
    (destination / "source-inventory.json").write_text(json.dumps({
        "schema": 1, "files": source_inventory}, indent=2) + "\n")
    inventory = []
    for path in sorted(destination.rglob("*")):
        if path.is_file():
            inventory.append({"path": path.relative_to(destination).as_posix(),
                              "size_bytes": path.stat().st_size, "sha256": sha256(path)})
    (destination / "inventory.json").write_text(json.dumps({"schema": 1, "files": inventory}, indent=2) + "\n")
    print(destination)


if __name__ == "__main__":
    main()
