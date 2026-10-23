#!/usr/bin/env python3
"""Invoke the external compiler without shell interpolation or source-tree fallbacks."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def build_command(action, environment, extra=()):
    if "PIPELANG_BIN" in environment:
        compiler = environment["PIPELANG_BIN"]
        if not Path(compiler).is_absolute() or not os.access(compiler, os.X_OK):
            raise ValueError("PIPELANG_BIN must name an absolute executable.")
    else:
        compiler = shutil.which("pipelang")
        if compiler is None:
            raise ValueError("Install the standalone PipeLang CLI first.")
    source = environment.get("PIPELANG_INPUT", "")
    if not source:
        raise ValueError("Set PIPELANG_INPUT to the .pipe entry file.")
    command = [compiler, action, "--in", source]
    if action == "compile":
        output = environment.get("PIPELANG_OUT", "")
        if not output:
            raise ValueError("Set PIPELANG_OUT to the generated artifact directory.")
        command += ["--out", output]
        if environment.get("PIPELANG_ENTRY"):
            command += ["--entry", environment["PIPELANG_ENTRY"]]
    elif action == "invoke":
        method = environment.get("PIPELANG_METHOD", "")
        if not method:
            raise ValueError("Set PIPELANG_METHOD to the public method.")
        command += ["--method", method]
        if environment.get("PIPELANG_CLASS"):
            command += ["--class", environment["PIPELANG_CLASS"]]
        try:
            arguments = json.loads(environment.get("PIPELANG_ARGS_JSON", "[]"))
        except json.JSONDecodeError as error:
            raise ValueError("PIPELANG_ARGS_JSON must be a JSON array of strings.") from error
        if not isinstance(arguments, list) or not all(isinstance(value, str) for value in arguments):
            raise ValueError("PIPELANG_ARGS_JSON must be a JSON array of strings.")
        for value in arguments:
            command += ["--arg", value]
    elif action != "check":
        raise ValueError("Unsupported language action.")
    return command + list(extra)


def main():
    if len(sys.argv) < 2:
        raise SystemExit("Select check, compile or invoke.")
    try:
        command = build_command(sys.argv[1], os.environ, sys.argv[2:])
    except ValueError as error:
        raise SystemExit(str(error)) from error
    raise SystemExit(subprocess.call(command))


if __name__ == "__main__":
    main()
