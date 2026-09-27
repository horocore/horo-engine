#!/usr/bin/env python3
"""Run the versioned, source-free extension author CI contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess  # nosec B404 - fixed SDK and CMake tools are the contract under test.
import sys
import tempfile
import tomllib
import xml.etree.ElementTree as xml


def run(*arguments: str) -> None:
    # Arguments are passed without a shell; tools originate in the pinned SDK or CI toolchain.
    # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit
    subprocess.run(arguments, check=True)  # nosec B603


def sdk_tool(root: Path, name: str) -> str:
    suffix = ".exe" if os.name == "nt" else ""
    tool = root / "bin" / (name + suffix)
    if not tool.is_file():
        raise ValueError(f"required SDK tool is missing: {name}")
    return str(tool)


def module_paths(stage: Path) -> tuple[Path, ...]:
    manifest = stage / "extension.json"
    document = json.loads(manifest.read_text(encoding="utf-8"))
    package_id = document.get("id")
    modules = document.get("modules")
    if not isinstance(package_id, str) or not package_id or not isinstance(modules, list) or not modules:
        raise ValueError("installed extension manifest has no package identity or native modules")
    binaries: list[Path] = []
    for module in modules:
        entry = module.get("entry") if isinstance(module, dict) else None
        if not isinstance(entry, str) or "\\" in entry or PurePosixPath(entry).parts[:1] != ("bin",) or \
                len(PurePosixPath(entry).parts) != 2 or entry != PurePosixPath(entry).as_posix():
            raise ValueError("installed extension manifest has an unsafe module entry")
        binary = stage / entry
        if not binary.is_file() or binary.is_symlink():
            raise ValueError(f"installed extension module is absent: {entry}")
        binaries.append(binary)
    return tuple(binaries)


def package_identity(stage: Path) -> str:
    manifest = json.loads((stage / "extension.json").read_text(encoding="utf-8"))
    with (stage / "horo-package.toml").open("rb") as source:
        package = tomllib.load(source)
    identity = package.get("package", {})
    if (package.get("schemaVersion") != 1 or not isinstance(identity, dict) or
            identity.get("id") != manifest.get("id") or not isinstance(identity.get("version"), str) or
            any(module.get("version") != identity["version"] for module in manifest.get("modules", []))):
        raise ValueError("installed package and extension manifest identities disagree")
    return identity["id"]


def require_executed_tests(path: Path) -> None:
    report = xml.parse(path).getroot()
    executed = [case for case in report.iter("testcase") if case.find("skipped") is None]
    if not executed:
        raise ValueError("CTest reported no executed extension tests")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--platform", choices=("linux-x64", "macos-arm64", "macos-x64", "windows-x64"), required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--sdk-sha256", required=True)
    args = parser.parse_args()
    try:
        if re.fullmatch(r"[0-9a-f]{40}", args.commit) is None or \
                re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository) is None or \
                re.fullmatch(r"[0-9a-f]{64}", args.sdk_sha256) is None:
            raise ValueError("invalid commit, repository or SDK digest attribution")
        sdk = args.sdk.resolve(strict=True)
        project = args.project.resolve(strict=True)
        output = args.output.absolute()
        if output.is_symlink():
            raise ValueError("artifact output cannot be a symlink")
        if output.exists() and (not output.is_dir() or any(output.iterdir())):
            raise ValueError("artifact output must be absent or empty")
        metadata = json.loads((sdk / "share/horo/extension-sdk/extension-sdk.json").read_text(encoding="utf-8"))
        sdk_version = metadata["sdk"]["version"]
        with tempfile.TemporaryDirectory(prefix="horo-extension-author-ci-") as temporary:
            scratch = Path(temporary)
            build, stage = scratch / "build", scratch / "stage"
            run("cmake", "-S", str(project), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
                f"-DHoroEngineExtensionSdk_DIR={sdk / 'lib/cmake/HoroEngineExtensionSdk'}")
            run("cmake", "--build", str(build), "--config", "Release", "--parallel", "2")
            run("ctest", "--test-dir", str(build), "-C", "Release", "--output-on-failure",
                "--output-junit", str(scratch / "ctest.xml"))
            require_executed_tests(scratch / "ctest.xml")
            run("cmake", "--install", str(build), "--config", "Release", "--prefix", str(stage))
            manifest = stage / "extension.json"
            run(sdk_tool(sdk, "horo-extension-validate"), "--json", "--schema-version", "1", str(manifest))
            for binary in module_paths(stage):
                run(sdk_tool(sdk, "horo-extension-conformance"), "--json", str(binary))
            if not (stage / "horo-package.toml").is_file():
                raise ValueError("project must install horo-package.toml for canonical packaging")
            package_id = package_identity(stage)
            archive = scratch / "extension.horopkg"
            package_tool = sdk_tool(sdk, "horo-package")
            run(package_tool, "pack", str(stage), str(archive))
            trust = scratch / "ci-integrity-only-trust.json"
            trust.write_text('{"schemaVersion":1,"allowUnsigned":true,"publishers":[]}', encoding="utf-8")
            run(package_tool, "verify", str(archive), "--package-id", package_id, "--trust", str(trust))
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            output.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(archive, output / "extension.horopkg")
            shutil.copyfile(scratch / "ctest.xml", output / "ctest.xml")
            provenance = {
                "schemaVersion": 1,
                "repository": args.repository,
                "commit": args.commit,
                "platform": args.platform,
                "sdkVersion": sdk_version,
                "sdkSha256": args.sdk_sha256,
                "packageId": package_id,
                "artifact": "extension.horopkg",
                "artifactSha256": digest,
                "verification": "archive-integrity-unsigned",
            }
            (output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError, KeyError, json.JSONDecodeError, subprocess.CalledProcessError, xml.ParseError) as error:
        print(f"extension author CI: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
