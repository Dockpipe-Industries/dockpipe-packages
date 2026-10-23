"""Fixed credential metadata only; endpoints and subprocess output stay private."""
import json
import os
from pathlib import Path
import re
import subprocess


def inspect_credentials(gdbus, address, expected_pid, expected_token, token_reader, output):
    report = {"schema": 1, "expected_pid": expected_pid,
              "service_pid_available": False, "service_pid_matches": False,
              "service_uid_available": False, "service_uid_matches": False,
              "fixture_token_stable": token_reader(expected_pid) == expected_token}

    def call(method, argument):
        result = subprocess.run([
            str(gdbus), "call", "--address", address,
            "--dest", "org.freedesktop.DBus", "--object-path", "/org/freedesktop/DBus",
            "--method", "org.freedesktop.DBus." + method, argument],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=4)
        if result.returncode or len(result.stdout) > 4096:
            return None
        return result.stdout.strip()

    owner_reply = call("GetNameOwner", "org.qemu")
    owner_match = re.fullmatch(r"\('(:[0-9]+\.[0-9]+)',\)", owner_reply or "")
    report["service_owner_available"] = owner_match is not None
    if owner_match:
        owner = owner_match[1]
        for method, prefix in (("GetConnectionUnixProcessID", "pid"),
                               ("GetConnectionUnixUser", "uid")):
            reply = call(method, owner)
            match = re.fullmatch(r"\(uint32 ([0-9]+),\)", reply or "")
            report["service_" + prefix + "_available"] = match is not None
            if match:
                value = int(match[1])
                report["observed_" + prefix] = value
                expected = expected_pid if prefix == "pid" else os.getuid()
                report["service_" + prefix + "_matches"] = value == expected
    Path(output).write_text(json.dumps(report, indent=2) + "\n")
