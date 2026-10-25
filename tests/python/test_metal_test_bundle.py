"""Provenance, archive confinement and CTest relocation for Metal bundles."""
from __future__ import annotations

import copy
import gzip
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tarfile

import pytest

from test_ci_test_isolation import run
from test_dependency_package import sdk
from test_metal_test_artifact import artifact_module as artifact

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("metal_test_bundle", ROOT / ".github/scripts/metal_test_bundle.py")
bundle = importlib.util.module_from_spec(SPEC)
sys.modules["dependency_package"] = sdk
SPEC.loader.exec_module(bundle)
IDENTITY = {"repository": "horocore/horo-engine", "run": 7, "attempt": 1,
            "commit": "a" * 40, "head": "b" * 40, "tree": "c" * 40}
MEMBER = "tests/HoroRenderMetalTests"


def native_binary(cpu: int = 0x0100000C, dependency: str | None = None) -> bytes:
    commands = struct.pack("<6I", 0x32, 24, 1, 14 << 16, 26 << 16, 0)
    count = 1
    if dependency:
        name = dependency.encode() + b"\0"
        size = (24 + len(name) + 7) // 8 * 8
        commands += struct.pack("<6I", 0xC, size, 24, 0, 0, 0) + name.ljust(size - 24, b"\0")
        count += 1
    return struct.pack("<8I", 0xFEEDFACF, cpu, 0, 2, count, len(commands), 0, 0) + commands


def manifest(data: bytes) -> dict:
    return {"schema": 1, "identity": IDENTITY, "selection": bundle.SELECTION,
            "files": {MEMBER: {"size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "executable": True}}}


def archive_at(path: Path, document: dict, entries: list) -> Path:
    with tarfile.open(path, "w:gz", format=tarfile.USTAR_FORMAT) as archive:
        for name, data, kind in [(bundle.MANIFEST, json.dumps(document).encode(), tarfile.REGTYPE), *entries]:
            info = tarfile.TarInfo(name)
            info.type, info.size = kind, len(data)
            archive.addfile(info, io.BytesIO(data))
    return path


def test_verified_regular_payload_keeps_bytes_and_uses_safe_modes(tmp_path: Path) -> None:
    data = native_binary()
    payload = archive_at(tmp_path / "payload.gz", manifest(data), [(MEMBER, data, tarfile.REGTYPE)])
    staging = tmp_path / "staging"
    staging.mkdir()
    bundle.unpack(payload, staging, IDENTITY)
    assert (staging / MEMBER).read_bytes() == data
    assert (staging / MEMBER).stat().st_mode & 0o7777 == 0o755


@pytest.mark.parametrize("attack", ["digest", "missing", "duplicate", "traversal", "absolute", "undeclared",
                                     "symlink", "hardlink", "device", "fifo", "sparse", "identity", "case"])
def test_invalid_payload_cannot_be_published(tmp_path: Path, attack: str) -> None:
    data = native_binary()
    document, entries = manifest(data), [(MEMBER, data, tarfile.REGTYPE)]
    if attack == "digest":
        entries[0] = (MEMBER, b"x" * len(data), tarfile.REGTYPE)
    elif attack == "missing":
        entries.clear()
    elif attack == "identity":
        document["identity"] = dict(IDENTITY, tree="d" * 40)
    elif attack == "case":
        record = document["files"].pop(MEMBER)
        document["files"] = {"apps/HoroEditor.app/Contents/Resources/A": record,
                             "apps/HoroEditor.app/Contents/Resources/a": record}
    else:
        name = {"duplicate": MEMBER, "traversal": "apps/HoroEditor.app/Contents/../outside",
                "absolute": str(tmp_path / "outside"), "undeclared": "CTestTestfile.cmake"}.get(attack, MEMBER)
        kind = {"symlink": tarfile.SYMTYPE, "hardlink": tarfile.LNKTYPE, "device": tarfile.CHRTYPE,
                "fifo": tarfile.FIFOTYPE, "sparse": tarfile.GNUTYPE_SPARSE}.get(attack, tarfile.REGTYPE)
        entries.append((name, data, kind))
    payload = archive_at(tmp_path / "invalid.gz", document, entries)
    staging = tmp_path / "quarantine"
    staging.mkdir()
    with pytest.raises((ValueError, tarfile.TarError)):
        bundle.unpack(payload, staging, IDENTITY)
    assert not (tmp_path / "live").exists()
    assert not (tmp_path / "outside").exists()


def test_pax_is_rejected_before_reading_its_large_declared_body(tmp_path: Path) -> None:
    member = tarfile.TarInfo("extended")
    member.type, member.size = tarfile.XHDTYPE, 64 * 1024 ** 2
    payload = tmp_path / "pax.gz"
    with gzip.open(payload, "wb") as output:
        output.write(member.tobuf(format=tarfile.USTAR_FORMAT))
    staging = tmp_path / "staging"
    staging.mkdir()
    with pytest.raises(ValueError, match="regular USTAR"):
        bundle.unpack(payload, staging, IDENTITY)


@pytest.mark.parametrize("limit", ["MAX_MEMBERS", "MAX_EXPANDED_BYTES", "MAX_METADATA_BYTES"])
def test_archive_bounds_precede_installation(tmp_path: Path, monkeypatch, limit: str) -> None:
    data = native_binary()
    payload = archive_at(tmp_path / "payload.gz", manifest(data), [(MEMBER, data, tarfile.REGTYPE)])
    monkeypatch.setattr(bundle, limit, 0)
    staging = tmp_path / "staging"
    staging.mkdir()
    with pytest.raises(ValueError):
        bundle.unpack(payload, staging, IDENTITY)


def test_macho_identity_checks_real_architecture_sdk_and_library_closure(tmp_path: Path) -> None:
    path = tmp_path / "native"
    path.write_bytes(native_binary(dependency="/System/Library/Frameworks/Metal.framework/Versions/A/Metal"))
    identity = bundle.macho_identity(path)
    assert identity["architecture"] == "arm64" and identity["minimum"] == 14 << 16 and identity["sdk"] == 26 << 16
    path.write_bytes(native_binary(cpu=0x01000007))
    with pytest.raises(ValueError, match="arm64"):
        bundle.macho_identity(path)
    path.write_bytes(native_binary(dependency="/opt/homebrew/lib/private.dylib"))
    with pytest.raises(ValueError, match="non-system"):
        bundle.macho_identity(path)


def test_typed_relocation_preserves_arguments_and_every_unrelated_property(tmp_path: Path) -> None:
    producer = "/producer checkout/build/ci-metal"
    test = {"name": "case [x]; ü", "command": [producer + "/tests/HoroRenderMetalTests", "case [x]; ü"],
            "properties": [{"name": "ENVIRONMENT_MODIFICATION", "value": ["HOME=set:" + producer + "/home"]},
                           {"name": "RESOURCE_LOCK", "value": ["original-lock"]},
                           {"name": "RUN_SERIAL", "value": True}, {"name": "TIMEOUT", "value": 60.0},
                           {"name": "WORKING_DIRECTORY", "value": producer + "/work"}]}
    before = copy.deepcopy(test)
    relocated = bundle.relocate_test(test, [producer], tmp_path)
    assert test == before
    assert relocated["command"] == [str(tmp_path / "tests/HoroRenderMetalTests"), "case [x]; ü"]
    assert relocated["properties"][1:4] == test["properties"][1:4]
    assert relocated["properties"][0]["value"] == ["HOME=set:" + str(tmp_path / "home")]
    test["properties"].append({"name": "UNKNOWN_PATH_PROPERTY", "value": producer + "/file"})
    with pytest.raises(ValueError, match="undeclared CTest property"):
        bundle.relocate_test(test, [producer], tmp_path)


def test_ctest_round_trip_retains_literal_arguments_and_execution_guards(tmp_path: Path) -> None:
    name = "literal [=[nested]=]; ü"
    test = {"name": name, "command": [sys.executable, "argument;with spaces"],
            "properties": [{"name": "ENVIRONMENT", "value": ["KEY=literal;semicolon"]},
                           {"name": "RESOURCE_LOCK", "value": ["original-lock"]},
                           {"name": "RUN_SERIAL", "value": True}, {"name": "SKIP_RETURN_CODE", "value": 4},
                           {"name": "TIMEOUT", "value": 60.0}, {"name": "WORKING_DIRECTORY", "value": str(tmp_path)}]}
    (tmp_path / "CTestTestfile.cmake").write_text(bundle.ctest_file([test]))
    document = json.loads(run("ctest", "--test-dir", str(tmp_path), "--show-only=json-v1").stdout)
    actual = document["tests"][0]
    assert actual["name"] == name and actual["command"] == test["command"]
    properties = {item["name"]: item["value"] for item in actual["properties"]}
    assert properties["RESOURCE_LOCK"] == ["original-lock"] and properties["ENVIRONMENT"] == ["KEY=literal;semicolon"]
    assert properties["RUN_SERIAL"] is True and properties["SKIP_RETURN_CODE"] == 4 and properties["TIMEOUT"] == 60


@pytest.mark.parametrize("field", list(IDENTITY))
def test_retained_producer_bundle_cannot_change_any_bound_source_field(field: str) -> None:
    document = manifest(native_binary())
    document["identity"] = dict(IDENTITY)
    document["identity"][field] = 9 if field in {"attempt", "run"} else "d" * 40
    consumer = dict(IDENTITY, attempt=2)
    expected = artifact.producer_identity(consumer, "1")
    with pytest.raises(ValueError, match="bundle identity"):
        bundle.validated_manifest(json.dumps(document).encode(), expected)


@pytest.mark.parametrize("body,returncode", [
    ('<testcase name="required"/>', 0),
    ('<testcase name="required"><skipped/></testcase>', 0),
    ('<testcase name="required"><failure/></testcase>', 0),
    ('<testcase name="required"><error/></testcase>', 0),
    ('<testcase name="other"/>', 0),
    ('<testcase name="required"/><testcase name="required"/>', 0),
    ('', 0),
    ('<testcase name="required"/>', 1),
])
def test_native_junit_requires_every_case_and_success(tmp_path: Path, body: str, returncode: int) -> None:
    report = tmp_path / "ctest.xml"
    report.write_text("<testsuite>" + body + "</testsuite>")
    tests = [{"name": "required"}]
    if body == '<testcase name="required"/>' and returncode == 0:
        assert bundle.qualification_report(report, tests, returncode) == {"tests": 1, "failures": 0, "skipped": 0}
    else:
        with pytest.raises(ValueError):
            bundle.qualification_report(report, tests, returncode)


@pytest.mark.parametrize("root", [None, 7, "relative/build/ci-metal", "/producer/build/other",
                                  "/producer/../build/ci-metal", "/producer//build/ci-metal",
                                  "/producer\\bad/build/ci-metal", "/producer\x00/build/ci-metal"])
def test_producer_root_must_be_an_absolute_canonical_build_directory(tmp_path: Path, root) -> None:
    document = {"tests": [], "producerRoots": [str(bundle.LOGICAL_SOURCE / bundle.BUILD), root]}
    with pytest.raises(ValueError, match="producer path contract"):
        bundle.validate_test_manifest(document, tmp_path)
