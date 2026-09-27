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
# The bounded parser below rejects DTD/entities and disables parameter entities.
# nosemgrep: python.lang.security.use-defused-xml.use-defused-xml
from xml.parsers import expat


MAXIMUM_JUNIT_BYTES = 8 * 1024 * 1024
SDK_TOOLS = frozenset(("horo-extension-validate", "horo-extension-conformance", "horo-package"))
MANIFEST_NAME = "extension.json"
JUNIT_NAME = "ctest.xml"
ARCHIVE_NAME = "extension.horopkg"
PROVENANCE_NAME = "provenance.json"


def run(sdk: Path, tool: str, *arguments: str) -> None:
    if tool in ("cmake", "ctest"):
        executable = tool
    elif tool in SDK_TOOLS:
        executable = sdk_tool(sdk, tool)
    else:
        raise ValueError(f"unsupported extension CI tool: {tool}")
    # Only named tools are executable. Project-derived values remain separate
    # argv elements, never shell text; SDK tools came from the digest-checked ZIP.
    # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-tainted-env-args.dangerous-subprocess-use-tainted-env-args,python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit
    subprocess.run((executable, *arguments), check=True, shell=False)  # nosec B603


def sdk_tool(root: Path, name: str) -> str:
    if name not in SDK_TOOLS:
        raise ValueError(f"unsupported extension SDK tool: {name}")
    suffix = ".exe" if os.name == "nt" else ""
    tool = root / "bin" / (name + suffix)
    if not tool.is_file() or tool.is_symlink():
        raise ValueError(f"required SDK tool is missing: {name}")
    return str(tool)


def module_paths(stage: Path) -> tuple[Path, ...]:
    manifest = stage / MANIFEST_NAME
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
    manifest = json.loads((stage / MANIFEST_NAME).read_text(encoding="utf-8"))
    with (stage / "horo-package.toml").open("rb") as source:
        package = tomllib.load(source)
    identity = package.get("package", {})
    if (package.get("schemaVersion") != 1 or not isinstance(identity, dict) or
            identity.get("id") != manifest.get("id") or not isinstance(identity.get("version"), str) or
            any(module.get("version") != identity["version"] for module in manifest.get("modules", []))):
        raise ValueError("installed package and extension manifest identities disagree")
    return identity["id"]


def require_executed_tests(path: Path) -> None:
    with path.open("rb") as source:
        report_bytes = source.read(MAXIMUM_JUNIT_BYTES + 1)
    if len(report_bytes) > MAXIMUM_JUNIT_BYTES:
        raise ValueError("CTest JUnit report exceeds the bounded parse size")
    parser = expat.ParserCreate()
    parser.SetParamEntityParsing(expat.XML_PARAM_ENTITY_PARSING_NEVER)
    case_open, skipped, executed = False, False, 0

    def reject_declaration(*_arguments: object) -> None:
        raise ValueError("CTest JUnit report cannot contain DTD or entity declarations")

    def start_element(name: str, _attributes: dict[str, str]) -> None:
        nonlocal case_open, skipped
        if name == "testcase":
            if case_open:
                raise ValueError("CTest JUnit report contains nested testcases")
            case_open, skipped = True, False
        elif name == "skipped" and case_open:
            skipped = True

    def end_element(name: str) -> None:
        nonlocal case_open, executed
        if name == "testcase":
            if not skipped:
                executed += 1
            case_open = False

    parser.StartDoctypeDeclHandler = reject_declaration
    parser.EntityDeclHandler = reject_declaration
    parser.ExternalEntityRefHandler = reject_declaration
    parser.StartElementHandler = start_element
    parser.EndElementHandler = end_element
    parser.Parse(report_bytes, True)
    if not executed:
        raise ValueError("CTest reported no executed extension tests")


def build_and_stage(sdk: Path, project: Path, scratch: Path) -> Path:
    build, stage = scratch / "build", scratch / "stage"
    run(sdk, "cmake", "-S", str(project), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
        f"-DHoroEngineExtensionSdk_DIR={sdk / 'lib/cmake/HoroEngineExtensionSdk'}")
    run(sdk, "cmake", "--build", str(build), "--config", "Release", "--parallel", "2")
    run(sdk, "ctest", "--test-dir", str(build), "-C", "Release", "--output-on-failure",
        "--output-junit", str(scratch / JUNIT_NAME))
    require_executed_tests(scratch / JUNIT_NAME)
    run(sdk, "cmake", "--install", str(build), "--config", "Release", "--prefix", str(stage))
    return stage


def package_and_publish(sdk: Path, stage: Path, scratch: Path, output: Path, args: argparse.Namespace,
                        sdk_version: str) -> None:
    manifest = stage / MANIFEST_NAME
    run(sdk, "horo-extension-validate", "--json", "--schema-version", "1", str(manifest))
    for binary in module_paths(stage):
        run(sdk, "horo-extension-conformance", "--json", str(binary))
    if not (stage / "horo-package.toml").is_file():
        raise ValueError("project must install horo-package.toml for canonical packaging")
    package_id = package_identity(stage)
    archive = scratch / ARCHIVE_NAME
    run(sdk, "horo-package", "pack", str(stage), str(archive))
    trust = scratch / "ci-integrity-only-trust.json"
    trust.write_text('{"schemaVersion":1,"allowUnsigned":true,"publishers":[]}', encoding="utf-8")
    run(sdk, "horo-package", "verify", str(archive), "--package-id", package_id, "--trust", str(trust))
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    output.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(archive, output / ARCHIVE_NAME)
    shutil.copyfile(scratch / JUNIT_NAME, output / JUNIT_NAME)
    provenance = {
        "schemaVersion": 1,
        "repository": args.repository,
        "commit": args.commit,
        "platform": args.platform,
        "sdkVersion": sdk_version,
        "sdkSha256": args.sdk_sha256,
        "packageId": package_id,
        "artifact": ARCHIVE_NAME,
        "artifactSha256": digest,
        "verification": "archive-integrity-unsigned",
    }
    (output / PROVENANCE_NAME).write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8")


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
        output = project / "ci-artifacts"
        if args.output.absolute() != output:
            raise ValueError("artifact output must be the author project's ci-artifacts")
        if output.is_symlink():
            raise ValueError("artifact output cannot be a symlink")
        if output.exists() and (not output.is_dir() or any(output.iterdir())):
            raise ValueError("artifact output must be absent or empty")
        metadata = json.loads((sdk / "share/horo/extension-sdk/extension-sdk.json").read_text(encoding="utf-8"))
        sdk_version = metadata["sdk"]["version"]
        with tempfile.TemporaryDirectory(prefix="horo-extension-author-ci-") as temporary:
            scratch = Path(temporary)
            stage = build_and_stage(sdk, project, scratch)
            package_and_publish(sdk, stage, scratch, output, args, sdk_version)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError,
            expat.ExpatError) as error:
        print(f"extension author CI: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
