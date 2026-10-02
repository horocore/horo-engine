"""Identify native CI cache namespaces without hashing engine source changes."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re


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


def compiler_identity(compiler: str, text: str) -> str:
    """Validate version output from the composite action's fixed tool probes."""
    if compiler == "cl":
        match = re.search(r"Compiler Version (\d+(?:\.\d+)+)", text)
        family = "msvc"
    elif compiler in {"clang", "clang++"}:
        match = re.search(r"clang version (\d+(?:\.\d+)+)", text)
        family = "appleclang" if "Apple clang" in text else "clang"
    elif compiler in {"gcc", "g++"}:
        match = re.fullmatch(r"(\d+(?:\.\d+)+)", text.strip())
        family = "gcc"
    else:
        raise ValueError(f"Unsupported CI compiler {compiler}")
    if match is None:
        raise ValueError(f"Cannot identify compiler {compiler}")
    return f"{family}-{match.group(1)}"


def main(argv: list[str] | None = None) -> None:
    """Write validated toolchain and dependency identities to GitHub step outputs."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--github-output", type=Path, required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--cc-version", required=True)
    parser.add_argument("--cxx-version", required=True)
    parser.add_argument("--cmake-version", required=True)
    args = parser.parse_args(argv)
    root = Path(__file__).resolve().parents[1]
    match = re.search(r"cmake version (\d+\.\d+)\.", args.cmake_version)
    if match is None:
        raise ValueError("Cannot identify CMake version")
    identities = {
        "compiler": compiler_identity(args.cxx, args.cxx_version),
        "c-compiler": compiler_identity(args.cc, args.cc_version),
        "cmake": match.group(1),
        "dependencies": dependency_fingerprint(root),
    }
    with args.github_output.open("a", encoding="utf-8") as output:
        for key, value in identities.items():
            print(f"{key}={value}", file=output)
    print(f"Compiler: {identities['compiler']}; C compiler: {identities['c-compiler']}; CMake: {identities['cmake']}")


if __name__ == "__main__":
    main()
