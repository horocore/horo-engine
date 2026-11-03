#!/usr/bin/env python3
"""Run the real native child case twice; this runner manufactures no snapshot or native artifact."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

CASE = "Physics canonical restart executes in separate verified same-binary processes"
def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()

def run_child(binary, directory, mode, expected):
    if digest(binary) != expected:
        raise RuntimeError("The actual executable changed between children")
    environment = dict(os.environ)
    environment.update(HORO_PHYSICS_RESTART_MODE=mode, HORO_PHYSICS_RESTART_DIRECTORY=str(directory))
    command = [str(binary), CASE, "--reporter", "junit", "--out", str(directory / (mode + ".xml"))]
    completed = subprocess.run(command, env=environment, cwd=directory, timeout=90, check=False)
    if completed.returncode:
        raise RuntimeError(f"Native {mode} child failed with exit {completed.returncode}; report {directory / (mode + '.xml')}")
    if digest(binary) != expected:
        raise RuntimeError("The actual executable changed while a child ran")
    report = ET.parse(directory / (mode + ".xml")).getroot()
    cases = list(report.iter("testcase"))
    if not cases or any(case.find(tag) is not None for case in cases for tag in ("skipped", "failure", "error")):
        raise RuntimeError("A real non-skipped passing native child case is required")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    binary = arguments.binary.resolve(strict=True)
    output = arguments.output.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise RuntimeError("A real compiled executable is required")
    output.mkdir(parents=True, exist_ok=True)
    expected = digest(binary)
    run_child(binary, output, "capture", expected)
    run_child(binary, output, "restore", expected)
    print(f"Both genuine native children passed; exact executable SHA256 {expected}")

if __name__ == "__main__":
    main()
