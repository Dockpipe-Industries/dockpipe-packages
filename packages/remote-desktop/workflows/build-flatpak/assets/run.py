#!/usr/bin/env python3
"""Run an explicit SDK build or Platform qualification stage."""
from pathlib import Path
import os
import subprocess
import sys


package = Path(__file__).resolve().parents[3]
stage = os.environ.get("DOCKPIPE_DESKTOP_FLATPAK_STAGE", "build")
if stage not in ("build", "qualify"):
    raise SystemExit("DOCKPIPE_DESKTOP_FLATPAK_STAGE must be build or qualify")
arguments = [sys.executable, str(package / "tooling/build_flatpak.py"), stage]
required = {"--output": "DOCKPIPE_DESKTOP_OUTPUT"}
if stage == "build":
    required.update({
        "--inputs": "DOCKPIPE_DESKTOP_DEPENDENCY_INPUTS",
        "--go": "DOCKPIPE_DESKTOP_GO",
        "--display-sdk": "DOCKPIPE_DESKTOP_FLATPAK_DISPLAY_SDK",
        "--display-sdk-release-sha256": "DOCKPIPE_DESKTOP_FLATPAK_DISPLAY_SDK_SHA256",
    })
for option, variable in required.items():
    value = os.environ.get(variable, "")
    if not value:
        raise SystemExit(variable + " must select an explicit producer input")
    arguments.extend([option, value])
if os.environ.get("DOCKPIPE_DESKTOP_ALLOW_DEVELOPMENT_SDK") == "1":
    arguments.append("--allow-development-sdk")
subprocess.run(arguments, check=True, timeout=1800)
