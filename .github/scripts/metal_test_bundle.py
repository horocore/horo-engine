"""Transfer the complete Metal CTest selection between identical source checkouts."""
# Only the producer compiles. The consumer verifies GitHub artifact provenance and
# every payload byte before installation or execution. CTest commands/properties
# retain their original values except explicitly typed build-local paths.
from __future__ import annotations

import argparse
import gzip
import io
import json
import os
import platform
from pathlib import Path
import re
import shutil
import stat
import struct
# Only fixed Git/CTest/OS probes and the verified native test selection run below.
import subprocess  # nosec B404
import tarfile
import tempfile
from defusedxml import ElementTree as element_tree

import dependency_package as package
import metal_test_artifact as artifact

REPOSITORY = artifact.REPOSITORY
LOGICAL_SOURCE = Path("/private/tmp/horo-metal-ci/source")
BUILD = "build/ci-metal"
MANIFEST = "horo-metal-tests.json"
PAYLOAD = "metal-tests.tar.gz"
SELECTION = (
    r"^(HoroRender(Display|Frontend|BackendRegistry|Metal)Tests::|"
    r"HoroEditorViewportMetalSmoke::|HoroMetalUploadShutdownSmoke::|"
    r"HoroEditorMetalFirstFrame$|HoroMetal(ShaderPreparation|GraphExecution)Smoke$)"
)
EXECUTABLES = {
    "HoroRenderDisplayTests", "HoroRenderFrontendTests", "HoroRenderBackendRegistryTests",
    "HoroRenderMetalTests", "HoroEditorViewportMetalSmoke", "HoroMetalUploadShutdownSmoke",
    "HoroMetalShaderPreparationSmoke", "HoroMetalGraphExecutionSmoke", "HoroEditor",
}
MAX_ARCHIVE_BYTES = artifact.MAX_ARCHIVE_BYTES
MAX_EXPANDED_BYTES = 2 * 1024 ** 3
MAX_MEMBERS = 10_000
MAX_METADATA_BYTES = artifact.MAX_METADATA_BYTES
PRIVATE_ENVIRONMENT = {"HOME", "USERPROFILE", "TMPDIR", "TMP", "TEMP"}
SHA = re.compile(r"[0-9a-f]{40}")


def command(arguments: list[str], *, cwd: Path | None = None) -> str:
    """Run fixed build-inspection tools, never shell text from an artifact."""
    executable = resolved_tool(arguments[0])
    return subprocess.run([executable, *arguments[1:]], cwd=cwd, check=True, text=True,  # nosec B603
                          capture_output=True, timeout=60).stdout


def resolved_tool(name: str) -> str:
    if name not in {"git", "ctest", "/usr/bin/sw_vers"}:
        raise ValueError("Unexpected inspection executable")
    path = shutil.which(name)
    if path is None or not Path(path).is_absolute():
        raise ValueError("A required native inspection tool is missing")
    return path


def source_identity(workspace: Path) -> tuple[str, str]:
    values = command(["git", "rev-parse", "HEAD", "HEAD^{tree}"], cwd=workspace).splitlines()
    if len(values) != 2 or any(not SHA.fullmatch(value) for value in values):
        raise ValueError("Invalid checkout identity")
    if command(["git", "diff", "--name-only", "HEAD", "--"], cwd=workspace).strip():
        raise ValueError("The checkout contains tracked changes")
    return values[0], values[1]


def context(workspace: Path) -> dict:
    """Bind the package to this event, run, attempt and immutable source tree."""
    commit, tree = source_identity(workspace)
    if os.environ.get("GITHUB_REPOSITORY") != REPOSITORY or commit != os.environ.get("GITHUB_SHA"):
        raise ValueError("Unexpected repository or checkout")
    run = os.environ.get("GITHUB_RUN_ID", "")
    attempt = os.environ.get("GITHUB_RUN_ATTEMPT", "")
    if not re.fullmatch(r"[1-9][0-9]*", run) or not re.fullmatch(r"[1-9][0-9]*", attempt):
        raise ValueError("Invalid workflow run identity")
    if os.environ.get("GITHUB_EVENT_NAME") not in {"pull_request", "push", "workflow_dispatch"}:
        raise ValueError("Unexpected source event")
    with Path(os.environ["GITHUB_EVENT_PATH"]).open("rb") as source:
        encoded = source.read(2 * 1024 ** 2 + 1)
    if len(encoded) > 2 * 1024 ** 2:
        raise ValueError("The source event exceeds its bound")
    event = json.loads(encoded, object_pairs_hook=package.unique_object)
    if event["repository"]["full_name"] != REPOSITORY:
        raise ValueError("Unexpected event repository")
    head = event["pull_request"]["head"]["sha"] if os.environ["GITHUB_EVENT_NAME"] == "pull_request" else commit
    if not SHA.fullmatch(head):
        raise ValueError("Invalid source head")
    return {"repository": REPOSITORY, "run": int(run), "attempt": int(attempt),
            "commit": commit, "tree": tree, "head": head}


def logical_source(workspace: Path) -> Path:
    """Create one owner-private stable alias without replacing an existing path."""
    parent = LOGICAL_SOURCE.parent
    parent.mkdir(mode=0o700, exist_ok=True)
    info = parent.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise ValueError("The logical source directory is not private")
    if LOGICAL_SOURCE.is_symlink():
        if LOGICAL_SOURCE.resolve(strict=True) != workspace:
            raise ValueError("The logical source alias belongs to another checkout")
    else:
        LOGICAL_SOURCE.symlink_to(workspace, target_is_directory=True)
    return LOGICAL_SOURCE


def build_relative(value: str, roots: list[str]) -> str:
    for root in roots:
        prefix = root.rstrip("/") + "/"
        if value.startswith(prefix):
            return package.portable_path(value[len(prefix):])
        if value == root:
            return "."
    raise ValueError("A runtime path is outside the producer build")


def relocate_directory(value: str, roots: list[str], build: Path) -> str:
    relative = build_relative(value, roots)
    return str(build if relative == "." else build / relative)


def relocate_environment(value: str, roots: list[str], build: Path, *, modification: bool) -> str:
    name, separator, setting = value.partition("=")
    if name in PRIVATE_ENVIRONMENT and separator:
        operation = "set:" if modification else ""
        if operation and not setting.startswith(operation):
            raise ValueError("A private environment operation is unsupported")
        path = setting[len(operation):]
        return name + "=" + operation + relocate_directory(path, roots, build)
    if any(root in value for root in roots):
        raise ValueError("An undeclared environment field contains a producer path")
    return value


def relocate_test(test: dict, roots: list[str], build: Path) -> dict:
    """Relocate only executable, working directory and private HOME/TMP fields."""
    result = {"name": test["name"], "command": list(test["command"]), "properties": []}
    result["command"][0] = relocate_directory(result["command"][0], roots, build)
    if any(root in argument for argument in result["command"][1:] for root in roots):
        raise ValueError("A command argument needs an explicit relocation contract")
    for prop in test.get("properties", []):
        name, value = prop["name"], prop["value"]
        if name == "WORKING_DIRECTORY":
            value = relocate_directory(value, roots, build)
        elif name in {"ENVIRONMENT", "ENVIRONMENT_MODIFICATION"}:
            value = [relocate_environment(item, roots, build, modification=name.endswith("MODIFICATION")) for item in value]
        elif any(root in json.dumps(value) for root in roots):
            raise ValueError("An undeclared CTest property contains a producer path")
        result["properties"].append({"name": name, "value": value})
    return result


def deployment_identity(entry: bytes) -> dict:
    """Decode a single macOS build-version load command."""
    if len(entry) < 24:
        raise ValueError("Invalid deployment command")
    target_platform, minimum, sdk, _ = struct.unpack_from("<4I", entry, 8)
    if target_platform != 1:
        raise ValueError("Invalid macOS deployment platform")
    return {"minimum": minimum, "sdk": sdk}


def system_library(entry: bytes) -> str:
    """Admit only a terminated system-library install name."""
    if len(entry) < 24:
        raise ValueError("Invalid library command")
    offset = struct.unpack_from("<I", entry, 8)[0]
    if not 24 <= offset < len(entry) or b"\0" not in entry[offset:]:
        raise ValueError("Invalid library path")
    name = entry[offset:].split(b"\0", 1)[0].decode("utf-8")
    if not name.startswith(("/System/Library/", "/usr/lib/")):
        raise ValueError("The executable requires an unstaged non-system library")
    return name


def macho_commands(path: Path) -> tuple[int, bytes]:
    """Bound the native arm64 executable's load-command table before parsing."""
    with path.open("rb") as stream:
        header = stream.read(32)
        if len(header) != 32:
            raise ValueError("Truncated Mach-O header")
        magic, cpu, _, kind, count, size, _, _ = struct.unpack("<8I", header)
        if magic != 0xFEEDFACF or cpu != 0x0100000C or kind != 2 or count > 4096 or size > 2 * 1024 ** 2:
            raise ValueError("Only native arm64 Mach-O executables are supported")
        data = stream.read(size)
    if len(data) != size:
        raise ValueError("Truncated Mach-O commands")
    return count, data


def macho_identity(path: Path) -> dict:
    """Read bounded Mach-O load commands without executing the binary."""
    count, data = macho_commands(path)
    position, deployment, libraries = 0, None, []
    for _ in range(count):
        if position + 8 > len(data):
            raise ValueError("Truncated load command")
        kind, length = struct.unpack_from("<II", data, position)
        if length < 8 or position + length > len(data):
            raise ValueError("Invalid load command length")
        entry = data[position:position + length]
        if kind == 0x32:
            if deployment is not None:
                raise ValueError("Invalid macOS deployment platform")
            deployment = deployment_identity(entry)
        elif kind in {0xC, 0x80000018, 0x8000001F, 0x80000023, 0x20}:
            libraries.append(system_library(entry))
        position += length
    if position != len(data) or deployment is None:
        raise ValueError("Missing or inconsistent deployment identity")
    return {"architecture": "arm64", **deployment, "libraries": libraries}


def discovery(build: Path) -> list[dict]:
    document = json.loads(command(["ctest", "--test-dir", str(build), "--show-only=json-v1", "-R", SELECTION]),
                          object_pairs_hook=package.unique_object)
    tests = document.get("tests", [])
    names = [test["name"] for test in tests]
    binaries = {Path(test["command"][0]).name for test in tests}
    if not tests or len(names) != len(set(names)) or binaries != EXECUTABLES:
        raise ValueError("Incomplete or duplicated Metal test selection")
    if any(not re.search(SELECTION, name) for name in names):
        raise ValueError("Unexpected Metal test name")
    return [{"name": test["name"], "command": test["command"], "properties": test.get("properties", [])} for test in tests]


def materialized_files(candidate: Path):
    """Enumerate declared runtime data without admitting source links or devices."""
    paths = candidate.rglob("*") if candidate.is_dir() else [candidate]
    for path in paths:
        if path.is_symlink():
            raise ValueError("Runtime inputs must be materialized regular files")
        if path.is_dir():
            continue
        if not path.is_file():
            raise ValueError("A declared runtime input is absent or special")
        yield path


def runtime_files(build: Path, tests: list[dict]) -> dict[str, Path]:
    """Stage executable/app resources and the existing generated shader/SDK data."""
    files = {}
    roots = [str(build), str(build.resolve(strict=True))]
    candidates = {build / build_relative(test["command"][0], roots) for test in tests}
    for binary in tuple(candidates):
        candidates.update(parent for parent in binary.parents if parent.suffix == ".app")
    candidates.update({build / "tests/MetalPreparationFixture.metallib", build / "sdk/gameplay"})
    for candidate in candidates:
        for path in materialized_files(candidate):
            relative = package.portable_path(path.relative_to(build).as_posix())
            files[relative] = path
    return files


def payload_path(name: str) -> str:
    """Confine the archive to the selected executables and declared runtime data."""
    package.portable_path(name)
    if (name in {"tests/" + binary for binary in EXECUTABLES - {"HoroEditor"}}
            or name == "tests/MetalPreparationFixture.metallib"
            or name.startswith(("apps/HoroEditor.app/Contents/", "sdk/gameplay/"))):
        return name
    raise ValueError("An undeclared file is outside the runtime payload")


def payload_inventory(files: dict[str, Path]) -> dict:
    """Bind runtime file contents and permissions within the package size budget."""
    table = {payload_path(name): {"size": path.stat().st_size, "sha256": package.digest_file(path),
                    "executable": bool(path.stat().st_mode & 0o111)} for name, path in files.items()}
    total = sum(record["size"] for record in table.values())
    if len(table) > MAX_MEMBERS or total > MAX_EXPANDED_BYTES:
        raise ValueError("Runtime payload exceeds its bounds")
    return table


def pack(workspace: Path, output: Path) -> dict:
    identity = context(workspace)
    build = logical_source(workspace) / BUILD
    tests = discovery(build)
    roots = [str(build), str(build.resolve(strict=True))]
    files = runtime_files(build, tests)
    table = payload_inventory(files)
    binaries = {build_relative(test["command"][0], roots) for test in tests}
    native = {name: macho_identity(build / name) for name in binaries}
    manifest = {"schema": 1, "identity": identity, "selection": SELECTION, "producerRoots": roots,
                "tests": tests, "files": table, "executables": native}
    encoded = json.dumps(manifest, sort_keys=True, allow_nan=False).encode()
    if len(encoded) > MAX_METADATA_BYTES:
        raise ValueError("Metal test metadata exceeds its bound")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(output, "w:gz", compresslevel=1, format=tarfile.USTAR_FORMAT) as archive:
        header = tarfile.TarInfo(MANIFEST)
        header.size = len(encoded)
        archive.addfile(header, io.BytesIO(encoded))
        for name, path in sorted(files.items()):
            archive.add(path, arcname=name, recursive=False)
    if output.stat().st_size > MAX_ARCHIVE_BYTES:
        raise ValueError("The compressed test bundle exceeds its bound")
    if context(workspace) != identity:
        raise ValueError("The source changed during packaging")
    return {"identity": identity, "testCount": len(tests), "bytes": output.stat().st_size,
            "sha256": package.digest_file(output)}


def cmake_literal(value: str) -> str:
    equals = "="
    while "]" + equals + "]" in value:
        equals += "="
    return "[" + equals + "[" + value + "]" + equals + "]"


def property_value(value) -> str:
    if isinstance(value, bool):
        return "TRUE" if value else "FALSE"
    if isinstance(value, (str, int, float)):
        return str(value)
    if isinstance(value, list):
        return ";".join(property_value(item).replace(";", "\\;") for item in value)
    raise ValueError("Unsupported CTest property value")


def ctest_file(tests: list[dict]) -> str:
    lines = ["# Generated from the verified, complete producer CTest discovery."]
    for test in tests:
        arguments = [cmake_literal(value) for value in [test["name"], *test["command"]]]
        lines.append("add_test(" + " ".join(arguments) + ")")
        for prop in test["properties"]:
            lines.append("set_tests_properties(" + cmake_literal(test["name"]) + " PROPERTIES "
                         + cmake_literal(prop["name"]) + " " + cmake_literal(property_value(prop["value"])) + ")")
    return "\n".join(lines) + "\n"


def validate_file_record(record: dict) -> int:
    """Validate a payload's exact size, digest and executable mode declaration."""
    size = record.get("size")
    if (not isinstance(size, int) or isinstance(size, bool) or size < 0
            or not re.fullmatch(r"[0-9a-f]{64}", record.get("sha256", ""))
            or not isinstance(record.get("executable"), bool)):
        raise ValueError("Invalid payload file record")
    return size


def validate_file_inventory(document: dict) -> None:
    """Require a bounded, unique set of declared runtime payload paths."""
    files = document.get("files")
    if not isinstance(files, dict) or not files or len(files) > MAX_MEMBERS:
        raise ValueError("Invalid Metal payload inventory")
    folded, total = set(), 0
    for name, record in files.items():
        payload_path(name)
        if name.casefold() in folded or name.casefold() == MANIFEST.casefold():
            raise ValueError("Duplicate or reserved payload path")
        folded.add(name.casefold())
        total += validate_file_record(record)
    if total > MAX_EXPANDED_BYTES:
        raise ValueError("The Metal payload exceeds its expanded size bound")


def validated_manifest(encoded: bytes, identity: dict) -> dict:
    """Bind format, source identity and file inventory before extracting bytes."""
    document = json.loads(encoded, object_pairs_hook=package.unique_object)
    exact_identity = json.dumps(document.get("identity"), sort_keys=True) == json.dumps(identity, sort_keys=True)
    if (not isinstance(document.get("schema"), int) or isinstance(document.get("schema"), bool)
            or document["schema"] != 1 or not exact_identity or document.get("selection") != SELECTION):
        raise ValueError("Incompatible Metal test bundle identity")
    validate_file_inventory(document)
    return document


def tar_header(source) -> tarfile.TarInfo | None:
    """Parse one fixed USTAR header; reject PAX/sparse/link extensions before reads."""
    header = source.read(tarfile.BLOCKSIZE)
    if len(header) != tarfile.BLOCKSIZE:
        raise ValueError("Truncated test bundle header")
    if not any(header):
        return None
    member = tarfile.TarInfo.frombuf(header, "utf-8", "strict")
    if member.type not in {tarfile.REGTYPE, tarfile.AREGTYPE} or member.size < 0:
        raise ValueError("Only regular USTAR payload members are supported")
    return member


def discard_padding(source, size: int) -> None:
    padding = (-size) % tarfile.BLOCKSIZE
    data = source.read(padding)
    if len(data) != padding or any(data):
        raise ValueError("Invalid test bundle padding")


def read_manifest(source, identity: dict) -> dict:
    """Require the bounded manifest as the first regular USTAR member."""
    first = tar_header(source)
    if first is None or first.name != MANIFEST or not 0 < first.size <= MAX_METADATA_BYTES:
        raise ValueError("Missing bounded Metal test manifest")
    encoded = source.read(first.size)
    if len(encoded) != first.size:
        raise ValueError("Truncated Metal test manifest")
    document = validated_manifest(encoded, identity)
    discard_padding(source, first.size)
    return document


def extract_verified_file(source, member, target: Path, record: dict) -> None:
    """Copy exactly one declared member and verify its digest before chmod."""
    target.parent.mkdir(parents=True, exist_ok=True)
    remaining = member.size
    with target.open("xb") as output:
        while remaining:
            chunk = source.read(min(remaining, 1024 * 1024))
            if not chunk:
                raise ValueError("Truncated Metal payload file")
            output.write(chunk)
            remaining -= len(chunk)
    discard_padding(source, member.size)
    if package.digest_file(target) != record["sha256"]:
        raise ValueError("Metal payload file digest mismatch")
    target.chmod(0o755 if record["executable"] else 0o644)


def unpack(payload: Path, staging: Path, identity: dict) -> dict:
    """Verify regular files in private quarantine before publishing the directory."""
    seen = set()
    with gzip.open(payload, "rb") as source:
        document = read_manifest(source, identity)
        files = document["files"]
        while member := tar_header(source):
            name = payload_path(member.name)
            record = files.get(name)
            if record is None or name in seen or member.size != record["size"]:
                raise ValueError("Unexpected, linked or duplicated Metal payload member")
            extract_verified_file(source, member, staging / name, record)
            seen.add(name)
        trailing = source.read(64 * 1024 + 1)
        if len(trailing) > 64 * 1024 or any(trailing):
            raise ValueError("Unexpected data after the declared test bundle")
    if seen != set(files):
        raise ValueError("The Metal payload is incomplete")
    return document


def validate_producer_roots(roots: list[str]) -> None:
    """Require both the logical and physical canonical producer build roots."""
    if not isinstance(roots, list) or len(roots) != 2 or roots[0] != str(LOGICAL_SOURCE / BUILD):
        raise ValueError("Unexpected producer path contract")
    validate_physical_build_root(roots[1])


def validate_physical_build_root(root: str) -> None:
    """Constrain the resolved producer directory to a canonical absolute build path."""
    if (not isinstance(root, str) or not root.startswith("/") or not root.endswith("/" + BUILD)
            or any(part in {".", "..", ""} for part in root.split("/")[1:])
            or "\\" in root or "\x00" in root):
        raise ValueError("Unexpected producer path contract")


def validate_test_manifest(document: dict, staging: Path) -> None:
    """Require the complete original executable set and unchanged native bytes."""
    tests, roots = document["tests"], document["producerRoots"]
    validate_producer_roots(roots)
    names = [test["name"] for test in tests]
    binaries = {build_relative(test["command"][0], roots) for test in tests}
    if not names or len(names) != len(set(names)) or {Path(name).name for name in binaries} != EXECUTABLES:
        raise ValueError("Incomplete or duplicated test executable selection")
    if any(not re.search(SELECTION, name) for name in names) or binaries != set(document["executables"]):
        raise ValueError("Unexpected test selection or binary inventory")
    validate_native_inventory(binaries, document, staging)


def validate_native_inventory(binaries: set[str], document: dict, staging: Path) -> None:
    """Require each selected executable's declared mode and unchanged Mach-O identity."""
    for name in binaries:
        if name not in document["files"] or not document["files"][name]["executable"]:
            raise ValueError("A selected executable is absent or not executable")
        if macho_identity(staging / name) != document["executables"][name]:
            raise ValueError("The Mach-O architecture, SDK or dependencies changed")


def validate_host(executables: dict) -> None:
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise ValueError("The runtime bundle requires a native arm64 Mac")
    components = [int(part) for part in command(["/usr/bin/sw_vers", "-productVersion"]).strip().split(".")]
    major, minor, patch = (components + [0, 0])[:3]
    current = major << 16 | minor << 8 | patch
    if any(record["minimum"] > current for record in executables.values()):
        raise ValueError("The host OS is older than a compiled deployment target")


def private_directories(tests: list[dict], build: Path) -> None:
    for test in tests:
        for prop in test["properties"]:
            if prop["name"] == "WORKING_DIRECTORY":
                Path(prop["value"]).mkdir(parents=True, exist_ok=True)
            elif prop["name"] in {"ENVIRONMENT", "ENVIRONMENT_MODIFICATION"}:
                for value in prop["value"]:
                    name, _, setting = value.partition("=")
                    if name in PRIVATE_ENVIRONMENT:
                        path = setting.removeprefix("set:")
                        if not Path(path).is_relative_to(build):
                            raise ValueError("A private fixture path escapes the bundle")
                        Path(path).mkdir(parents=True, exist_ok=True)


def verify_payload_files(build: Path, files: dict) -> None:
    for name, record in files.items():
        path = build / payload_path(name)
        if path.is_symlink() or not path.is_file() or package.digest_file(path) != record["sha256"]:
            raise ValueError("An installed runtime file changed")


def qualification_report(path: Path, tests: list[dict], returncode: int) -> dict:
    """Require fresh JUnit to account for every case without failures or skips."""
    with path.open("rb") as source:
        junit = source.read(MAX_METADATA_BYTES + 1)
    if len(junit) > MAX_METADATA_BYTES:
        raise ValueError("The native JUnit report exceeds its bound")
    cases = list(element_tree.fromstring(junit, forbid_dtd=True).iter("testcase"))
    expected = sorted(test["name"] for test in tests)
    if sorted(case.get("name", "") for case in cases) != expected:
        raise ValueError("JUnit does not contain the complete producer test selection")
    unsuccessful = [case.get("name") for case in cases
                    if any(case.find(tag) is not None for tag in ("failure", "error", "skipped"))]
    if returncode != 0 or unsuccessful:
        raise ValueError("Metal hardware qualification failed or skipped a required case")
    return {"tests": len(cases), "failures": 0, "skipped": 0}


def run_tests(workspace: Path, build: Path, tests: list[dict], files: dict, identity: dict) -> dict:
    """Run every discovered case with the original limits and verify actual JUnit."""
    verify_payload_files(build, files)
    report = build / "ctest.xml"
    if report.exists() or report.is_symlink():
        raise ValueError("A previous native JUnit report already exists")
    environment = dict(os.environ, MTL_DEBUG_LAYER="1")
    outcome = subprocess.run([resolved_tool("ctest"), "--test-dir", str(build), "--parallel", "1", "--timeout", "90",  # nosec B603
                              "--output-on-failure", "--no-tests=error", "-R", SELECTION,
                              "--output-junit", "ctest.xml"], cwd=workspace, env=environment, check=False)
    if context(workspace) != identity:
        raise ValueError("The source changed while running Metal tests")
    verify_payload_files(build, files)
    return qualification_report(report, tests, outcome.returncode)


def consume(workspace: Path, artifact_id: str, artifact_digest: str, payload_digest: str,
            producer_name: str, producer_attempt: str) -> dict:
    identity = context(workspace)
    produced = artifact.producer_identity(identity, producer_attempt)
    provenance = artifact.artifact_provenance(produced, artifact_id, artifact_digest, producer_name)
    destination = workspace / BUILD
    if destination.exists():
        raise ValueError("A consumer test installation already exists")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=destination.parent) as temporary:
        quarantine = Path(temporary)
        archive, payload, staging = quarantine / "artifact.zip", quarantine / PAYLOAD, quarantine / "verified"
        staging.mkdir()
        artifact.download_artifact(artifact_id, archive, provenance["artifact"]["size_in_bytes"], artifact_digest)
        artifact.unwrap_artifact(archive, payload, payload_digest)
        document = unpack(payload, staging, produced)
        validate_test_manifest(document, staging)
        validate_host(document["executables"])
        logical_build = logical_source(workspace) / BUILD
        tests = [relocate_test(test, document["producerRoots"], logical_build) for test in document["tests"]]
        if context(workspace) != identity:
            raise ValueError("The source changed before bundle installation")
        staging.rename(destination)
    private_directories(tests, logical_build)
    (destination / "CTestTestfile.cmake").write_text(ctest_file(tests))
    actual = discovery(logical_build)
    if sorted(actual, key=lambda test: test["name"]) != sorted(tests, key=lambda test: test["name"]):
        raise ValueError("Consumer CTest commands or properties differ from the producer")
    result = run_tests(workspace, logical_build, tests, document["files"], identity)
    return {"identity": identity, "artifact": int(artifact_id), "producerJob": provenance["producer"],
            "producerAttempt": produced["attempt"], "testCount": len(tests),
            "build": str(logical_build), "qualification": result}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("alias", "pack", "consume"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--artifact-id")
    parser.add_argument("--artifact-digest")
    parser.add_argument("--payload-digest")
    parser.add_argument("--producer-name")
    parser.add_argument("--producer-attempt")
    arguments = parser.parse_args()
    workspace = Path(os.environ["GITHUB_WORKSPACE"]).resolve(strict=True)
    if arguments.operation == "alias":
        print(logical_source(workspace))
    elif arguments.operation == "pack":
        if arguments.output is None:
            parser.error("pack requires --output")
        print(json.dumps(pack(workspace, arguments.output), sort_keys=True))
    else:
        if not all((arguments.artifact_id, arguments.artifact_digest, arguments.payload_digest,
                    arguments.producer_name, arguments.producer_attempt)):
            parser.error("consume requires immutable artifact identity and producer outputs")
        print(json.dumps(consume(workspace, arguments.artifact_id, arguments.artifact_digest,
                                 arguments.payload_digest, arguments.producer_name,
                                 arguments.producer_attempt), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
