#!/usr/bin/env python3
"""Run package fixtures; native rendering/capture qualification is separate."""
from pathlib import Path
import subprocess
import sys

package = Path(__file__).resolve().parents[1]
subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", str(package / "tests"),
                "-p", "test_*.py", "-v"], check=True, timeout=90)
