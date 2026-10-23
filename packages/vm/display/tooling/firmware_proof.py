#!/usr/bin/env python3
"""Bounded local firmware fixture; invoke through a Dockpipe host workflow."""
import argparse
import ctypes
import platform
import struct
import tempfile
from urllib.parse import quote
from xml.sax.saxutils import escape
import json
import os
from pathlib import Path
import subprocess
import time
import sys

sys.dont_write_bytecode = True


def wait_socket(path, process):
    deadline = time.monotonic() + 5
    while not path.exists():
        if process.poll() is not None or time.monotonic() > deadline:
            raise RuntimeError("Fixture private bus did not become ready")
        time.sleep(0.05)


def process_start_token(pid):
    if platform.system() == "Windows":
        from windows_fixture import process_start_token as windows_token
        return windows_token(pid)
    if platform.system() == "Darwin":
        # proc_bsdinfo public ABI: fixed 136-byte layout on Darwin amd64/arm64.
        # Inspect only PID/UID/status and the two start-time fields; never decode
        # the process-name buffers occupying the middle of this structure.
        library = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
        library.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64,
                                        ctypes.c_void_p, ctypes.c_int]
        library.proc_pidinfo.restype = ctypes.c_int
        buffer = ctypes.create_string_buffer(136)
        count = library.proc_pidinfo(pid, 3, 0, buffer, len(buffer))
        if count != len(buffer):
            raise RuntimeError("Cannot read owned fixture process identity")
        status = struct.unpack_from("=I", buffer.raw, 4)[0]
        actual_pid = struct.unpack_from("=I", buffer.raw, 12)[0]
        uid = struct.unpack_from("=I", buffer.raw, 20)[0]
        seconds, microseconds = struct.unpack_from("=QQ", buffer.raw, 120)
        if actual_pid != pid or uid != os.getuid() or status == 5 or microseconds >= 1000000:
            raise RuntimeError("Fixture process identity mismatch")
        return str(seconds) + ":" + str(microseconds)
    if platform.system() == "Linux":
        stat = Path(f"/proc/{pid}/stat").read_text()
        return stat[stat.rfind(")") + 2:].split()[19]
    raise ValueError("This firmware fixture currently supports Linux and macOS")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--dbus-daemon", type=Path,
                        default=Path("/opt/homebrew/bin/dbus-daemon") if platform.system() == "Darwin" else Path("/usr/bin/dbus-daemon"))
    parser.add_argument("--firmware-directory", type=Path, required=True)
    parser.add_argument("--frontend", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--raw-only", action="store_true")
    parser.add_argument("--credential-probe-gdbus", type=Path)
    parser.add_argument("--harness", type=Path)
    parser.add_argument("--package-root", type=Path)
    args = parser.parse_args()
    for executable in (args.qemu, args.frontend, args.dbus_daemon):
        if not executable.is_absolute() or not executable.is_file():
            raise ValueError("Fixture requires explicit executable paths")
    args.output = args.output.resolve()
    args.output.mkdir(mode=0o700)
    windows = platform.system() == "Windows"
    if windows:
        from windows_fixture import protect_path
        protect_path(args.output, directory=True)
    runtime_parent = args.output.parent if windows else Path("/tmp").resolve()
    runtime = tempfile.TemporaryDirectory(prefix="dp-display-", dir=str(runtime_parent))
    runtime_path = Path(runtime.name).resolve()
    if windows:
        protect_path(runtime_path, directory=True)
    socket = runtime_path / "bus"
    if len(str(socket).encode("utf-8")) > 100:
        raise ValueError("Fixture runtime parent must keep the private socket path within 100 bytes")
    address = "unix:path=" + quote(str(socket).replace("\\", "/"), safe="/")
    configuration = runtime_path / "bus.conf"
    configuration.write_text('<busconfig><type>session</type><auth>EXTERNAL</auth>'
        '<listen>' + escape(address) + '</listen>'
        '<policy context="default"><allow send_destination="*"/><allow receive_sender="*"/>'
        '<allow own="*"/></policy></busconfig>')
    if windows:
        protect_path(configuration)
    processes = []
    try:
        with (args.output / "bus.log").open("wb") as bus_log, \
             (args.output / "qemu.log").open("wb") as qemu_log, \
             (args.output / "frontend.log").open("wb") as frontend_log:
            bus = subprocess.Popen([
                str(args.dbus_daemon), "--nofork", "--nopidfile",
                "--config-file=" + str(configuration), "--address=" + address], stdout=bus_log, stderr=subprocess.STDOUT)
            processes.append(bus)
            wait_socket(socket, bus)
            qemu_arguments = [str(args.qemu), "-machine", "pc", "-accel", "tcg",
                "-m", "128", "-nodefaults", "-no-user-config", "-nic", "none", "-vga", "std",
                "-device", "qemu-xhci", "-device", "usb-tablet",
                "-display", "dbus,addr=" + address + ",gl=off",
                "-monitor", "none", "-serial", "none", "-no-reboot",
                "-L", str(args.firmware_directory)]
            (args.output / "fixture.json").write_text(json.dumps({
                "schema": 1, "qemu_argv": qemu_arguments,
                "os_disk": False, "network": False, "display_protocol": "local-dbus",
                "maximum_seconds": 45}, indent=2) + "\n")
            qemu = subprocess.Popen(qemu_arguments, stdout=qemu_log, stderr=subprocess.STDOUT)
            processes.append(qemu)
            # Attach after SeaBIOS has selected its stable text-mode scanout.
            time.sleep(3)
            if qemu.poll() is not None:
                raise RuntimeError("Firmware fixture exited before display listener")
            start_token = process_start_token(qemu.pid)
            if args.credential_probe_gdbus:
                from credential_diagnostic import inspect_credentials
                inspect_credentials(args.credential_probe_gdbus, address, qemu.pid,
                                    start_token, process_start_token,
                                    args.output / "credential-diagnostic.json")
            if args.harness:
                if not args.package_root or not args.package_root.is_absolute():
                    raise ValueError("Factory proof needs an absolute installed package root")
                resource = "firmware-fixture"
                state = args.output / ("display-" + resource)
                state.mkdir(mode=0o700)
                if windows:
                    protect_path(state, directory=True)
                metadata = {"schema": 1, "resource_id": resource, "backend": "qemu-dbus",
                    "bus_address": address, "console_id": 0, "qemu_pid": qemu.pid,
                    "qemu_start_token": start_token}
                descriptor = state / "display.json"
                fd = os.open(descriptor, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(fd, "w") as stream:
                    json.dump(metadata, stream)
                if windows:
                    protect_path(descriptor)
                library_names = {"Darwin": "libdockpipe-vm-display.dylib",
                                 "Windows": "dockpipe-vm-display.dll",
                                 "Linux": "libdockpipe-vm-display.so"}
                library_name = library_names[platform.system()]
                config = {"package_root": str(args.package_root.resolve()),
                    "library": str(args.package_root.resolve() / "assets/display" / library_name),
                    "lease_id": resource, "options": {"schema": 1, "resource_id": resource,
                    "state_ref": str(state)}}
                configuration = args.output / "harness.json"
                configuration.write_text(json.dumps(config))
                command = [str(args.harness), str(configuration), str(args.output)]
            else:
                command = [str(args.frontend), "--bus-address", address,
                    "--expected-qemu-pid", str(qemu.pid), "--expected-qemu-start-token", start_token,
                    "--proof-directory", str(args.output), "--proof-input"]
            if args.raw_only:
                if args.harness:
                    raise ValueError("Raw-only protocol proof uses the standalone frontend")
                command.append("--raw-only")
            result = subprocess.run(command,
                stdout=frontend_log, stderr=subprocess.STDOUT, timeout=30)
            (args.output / "frontend-exit.json").write_text(json.dumps({
                "schema": 1, "return_code": result.returncode}) + "\n")
            if result.returncode:
                raise RuntimeError("Local display proof failed with child return code " + str(result.returncode))
            if qemu.poll() is not None:
                raise RuntimeError("Display close unexpectedly stopped fixture QEMU")
    finally:
        # Only exact child handles from this fixture are signalled. Existing VMs,
        # buses, disks, host input and other desktop processes are never selected.
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)
        runtime.cleanup()
        (args.output / "fixture-cleanup.json").write_text(json.dumps({
            "schema": 1, "processes_started": len(processes),
            "all_children_exited": all(process.poll() is not None for process in processes),
            "owned_child_return_codes": [process.returncode for process in processes],
            "private_runtime_removed": not runtime_path.exists(),
            "existing_vm_touched": False}, indent=2) + "\n")


if __name__ == "__main__":
    main()
