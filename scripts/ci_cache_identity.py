"""Identify native CI cache namespaces without hashing engine source changes."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess


def dependency_fingerprint(root: Path) -> str:
    """Hash declarations and capability defaults, including file names and CRLF normalization."""
    paths = {
        root / "CMakeLists.txt",
        root / "cmake" / "Dependencies.cmake",
        *root.glob("cmake/Horo*Dependency.cmake"),
        *root.glob("cmake/Horo*DependencyPolicy.cmake"),
        root / "cmake" / "HoroGnsProtobufConfig.cmake",
    }
    digest = hashlib.sha256(b"horo-ci-dependencies-v1\0")
    for path in sorted(paths):
        digest.update(path.relative_to(root).as_posix().encode("utf-8") + b"\0")
        digest.update(path.read_bytes().replace(b"\r\n", b"\n") + b"\0")
    return digest.hexdigest()


def compiler_identity(compiler: str) -> str:
    """Read the active GCC, Clang or MSVC version after toolchain setup."""
    msvc = Path(compiler).stem.lower() == "cl"
    result = subprocess.run(
        [compiler] if msvc else [compiler, "--version"],
        capture_output=True,
        text=True,
        errors="replace",
        check=False,
    )
    text = result.stdout + result.stderr
    if msvc:
        match = re.search(r"Compiler Version (\d+(?:\.\d+)+)", text)
        family = "msvc"
    elif "clang" in text.lower():
        match = re.search(r"clang version (\d+(?:\.\d+)+)", text)
        family = "appleclang" if "Apple clang" in text else "clang"
    else:
        version = subprocess.check_output([compiler, "-dumpfullversion", "-dumpversion"], text=True).strip()
        match = re.fullmatch(r"(\d+(?:\.\d+)+)", version)
        family = "gcc"
    if match is None or (not msvc and result.returncode != 0):
        raise ValueError(f"Cannot identify compiler {compiler}")
    return f"{family}-{match.group(1)}"


def main() -> None:
    """Write validated toolchain and dependency identities to GitHub step outputs."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--github-output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    cmake_version = subprocess.check_output(["cmake", "--version"], text=True)
    match = re.search(r"cmake version (\d+\.\d+)\.", cmake_version)
    if match is None:
        raise ValueError("Cannot identify CMake version")
    identities = {
        "compiler": compiler_identity(os.environ["CXX"]),
        "c-compiler": compiler_identity(os.environ["CC"]),
        "cmake": match.group(1),
        "dependencies": dependency_fingerprint(root),
    }
    with args.github_output.open("a", encoding="utf-8") as output:
        for key, value in identities.items():
            print(f"{key}={value}", file=output)
    print(f"Compiler: {identities['compiler']}; C compiler: {identities['c-compiler']}; CMake: {identities['cmake']}")


if __name__ == "__main__":
    main()
