#!/usr/bin/env python3
"""Resolve the candidate media closure locally without opening a device."""
from pathlib import Path
import argparse
import ctypes
import json
import os


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--module", required=True)
arguments = parser.parse_args()
module = Path(arguments.module)
if not module.is_absolute() or module.is_symlink() or not module.is_file():
    parser.error("--module must select a regular absolute native fixture")
library = ctypes.CDLL(str(module), mode=os.RTLD_NOW | os.RTLD_LOCAL)
library.desktop_media_probe.restype = ctypes.c_int
if library.desktop_media_probe() != 1:
    raise SystemExit("Selected native media ABI or H264 decoder is unavailable")
print(json.dumps({"load": "RTLD_NOW|RTLD_LOCAL", "result": "pass",
                  "decoder": "H264", "capture_or_input_performed": False}))
