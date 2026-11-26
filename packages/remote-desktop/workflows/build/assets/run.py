#!/usr/bin/env python3
"""Run the package producer with explicit operator-selected inputs."""
from pathlib import Path
import os
import subprocess
import sys


package = Path(__file__).resolve().parents[3]
arguments = [sys.executable, str(package / "tooling/build.py")]
required = {
    "--output": "DOCKPIPE_DESKTOP_OUTPUT",
    "--inputs": "DOCKPIPE_DESKTOP_DEPENDENCY_INPUTS",
    "--go": "DOCKPIPE_DESKTOP_GO",
}
for option, variable in required.items():
    value = os.environ.get(variable, "")
    if not value:
        raise SystemExit(variable + " must select an explicit absolute producer input")
    arguments.extend([option, value])
qt = os.environ.get("DOCKPIPE_DESKTOP_QT_PREFIX")
if qt:
    arguments.extend(["--qt-prefix", qt])
subprocess.run(arguments, check=True, timeout=900)
