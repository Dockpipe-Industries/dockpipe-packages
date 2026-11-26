#!/usr/bin/env python3
"""Materialize the candidate through the catalog's generic package compiler."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile


package = Path(__file__).resolve().parents[2]
repository = package.parents[1]
cli = Path(os.environ.get("DOCKPIPE_BIN", ""))
if not cli.is_absolute() or not cli.is_file():
    raise SystemExit("DOCKPIPE_BIN must select the pinned released catalog producer")
required = {"--inputs": "DOCKPIPE_DESKTOP_DEPENDENCY_INPUTS", "--go": "DOCKPIPE_DESKTOP_GO"}
arguments = [sys.executable, str(package / "tooling/build.py")]
for option, variable in required.items():
    value = os.environ.get(variable, "")
    if not value or not Path(value).is_absolute():
        raise SystemExit(variable + " must select an absolute producer input")
    arguments.extend([option, value])
qt = os.environ.get("DOCKPIPE_DESKTOP_QT_PREFIX")
if qt:
    arguments.extend(["--qt-prefix", qt])
state = subprocess.check_output([str(cli), "__state", "package-runtime", "--workdir", str(repository),
                                 "--owner", "remote-desktop", "--path", "native-build"], text=True).strip()
state_root = Path(state)
if not state_root.is_absolute():
    raise SystemExit("Package runtime did not return an absolute build scope")
state_root.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="native-", dir=state_root) as temporary:
    candidate = Path(temporary) / "candidate"
    subprocess.run([*arguments, "--output", str(candidate)], check=True, timeout=900)
    staging = Path(temporary) / "staging/resolvers/dockpipe.desktop.native"
    staging.parent.mkdir(parents=True)
    (candidate / "resolver").rename(staging)
    subprocess.run([str(cli), "package", "compile", "resolvers", "--workdir", str(repository),
                    "--from", str(staging.parents[1]), "--force"], check=True, timeout=120)
