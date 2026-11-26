#!/usr/bin/env python3
"""Run the owning package's self-contained contract fixtures."""
from pathlib import Path
import subprocess
import sys


package = Path(__file__).resolve().parents[3]
subprocess.run([sys.executable, str(package / "tests/run.py")], check=True, timeout=90)
