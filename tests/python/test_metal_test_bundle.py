"""Provenance, archive confinement and CTest relocation for Metal bundles."""
from __future__ import annotations

import copy
import gzip
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tarfile
from types import SimpleNamespace

import pytest

from test_ci_test_isolation import run
from test_dependency_package import sdk
from test_metal_test_artifact import artifact_module as artifact, native_binary

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("metal_test_bundle", ROOT / ".github/scripts/metal_test_bundle.py")
bundle = importlib.util.module_from_spec(SPEC)
sys.modules["dependency_package"] = sdk
SPEC.loader.exec_module(bundle)
IDENTITY = {"repository": "horocore/horo-engine", "run": 7, "attempt": 1,
            "commit": "a" * 40, "head": "b" * 40, "tree": "c" * 40}
MEMBER = "tests/HoroRenderMetalTests"


def test_git_inspection_preserves_literal_arguments_and_resolved_executable(tmp_path: Path, monkeypatch) -> None:
    executable = str(tmp_path / "native tools ü" / "git")
    calls = []
    monkeypatch.setattr(bundle.shutil, "which", lambda _name: executable)

    def inspect(arguments, **options):
        calls.append((arguments, options))
        return SimpleNamespace(stdout="source identity")

    monkeypatch.setattr(bundle.subprocess, "run", inspect)
    arguments = ["rev-parse", "literal; $(not a shell) ü"]
    assert bundle.git_output(arguments, cwd=tmp_path) == "source identity"
    assert calls == [(["git", *arguments], {"executable": executable, "cwd": tmp_path,
                      "check": True, "text": True, "capture_output": True, "timeout": 60, "shell": False})]


@pytest.mark.parametrize("tool", ["git", "ctest"])
@pytest.mark.parametrize("resolved", [None, "relative/tool"])
def test_inspection_rejects_missing_or_relative_executables(monkeypatch, tool: str, resolved: str | None) -> None:
    monkeypatch.setattr(bundle.shutil, "which", lambda _name: resolved)
    with pytest.raises(ValueError, match="inspection tool"):
        bundle.resolved_tool(tool)


@pytest.mark.parametrize("available", [True, False])
def test_host_inspection_uses_the_fixed_native_program(monkeypatch, available: bool) -> None:
    monkeypatch.setattr(bundle.platform, "system", lambda: "Darwin")
    monkeypatch.setattr(bundle.platform, "machine", lambda: "arm64")
    calls = []

    def inspect(arguments, **options):
        calls.append((arguments, options))
        if not available:
            raise FileNotFoundError("Native OS inspection is unavailable")
        return SimpleNamespace(stdout="26.0\n")

    monkeypatch.setattr(bundle.subprocess, "run", inspect)
    executables = {"native": {"minimum": 0}}
    if available:
        bundle.validate_host(executables)
    else:
        with pytest.raises(FileNotFoundError):
            bundle.validate_host(executables)
    assert calls == [(["/usr/bin/sw_vers", "-productVersion"], {
        "check": True, "text": True, "capture_output": True, "timeout": 60, "shell": False})]


def test_native_ctest_keeps_complete_selection_and_verified_payload(tmp_path: Path, monkeypatch) -> None:
    build = tmp_path / "native build ü"
    payload = build / MEMBER
    payload.parent.mkdir(parents=True)
    payload.write_bytes(b"verified native bytes")
    files = {MEMBER: {"sha256": hashlib.sha256(payload.read_bytes()).hexdigest()}}
    for name in bundle.UI_PAINT_FILES:
        path = build / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"verified UI qualification input")
        files[name] = {"sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    executable = str(tmp_path / "native tools ü" / "ctest")
    calls = []
    monkeypatch.setattr(bundle.shutil, "which", lambda _name: executable)
    monkeypatch.setattr(bundle, "context", lambda _workspace: IDENTITY)
    monkeypatch.setenv("HORO_UI_PAINT_METAL_FIXTURE", "/untrusted/producer/path")

    def execute(arguments, **options):
        calls.append((arguments, options))
        (build / "ctest.xml").write_text('<testsuite><testcase name="required"/></testsuite>')
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(bundle.subprocess, "run", execute)
    assert bundle.run_tests(tmp_path, build, [{"name": "required"}], files, IDENTITY) == {
        "tests": 1, "failures": 0, "skipped": 0}
    arguments, options = calls[0]
    assert arguments == ["ctest", "--test-dir", str(build), "--parallel", "1", "--timeout", "90",
                         "--output-on-failure", "--no-tests=error", "-R", bundle.SELECTION,
                         "--output-junit", "ctest.xml"]
    assert options["executable"] == executable
    assert options["cwd"] == tmp_path
    assert options["shell"] is False
    assert options["check"] is False
    assert options["env"]["MTL_DEBUG_LAYER"] == "1"
    assert options["env"]["HORO_UI_PAINT_METAL_FIXTURE"] == str(build / bundle.UI_PAINT_FIXTURE)
    assert payload.read_bytes() == b"verified native bytes"


@pytest.mark.parametrize("damage", ["omitted", "missing", "tampered"])
def test_incomplete_ui_fixture_prevents_native_execution(tmp_path: Path, monkeypatch, damage: str) -> None:
    files = {}
    for name in bundle.UI_PAINT_FILES:
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"verified UI data")
        files[name] = {"sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    name = sorted(bundle.UI_PAINT_FILES)[0]
    if damage == "omitted":
        del files[name]
    elif damage == "missing":
        (tmp_path / name).unlink()
    else:
        (tmp_path / name).write_bytes(b"changed UI data")
    monkeypatch.setattr(bundle.subprocess, "run", lambda *_args, **_kwargs: pytest.fail("Invalid fixture reached CTest"))
    with pytest.raises(ValueError, match="qualification inputs|runtime file"):
        bundle.run_tests(tmp_path, tmp_path, [], files, IDENTITY)


def test_graph_cases_have_individual_ctest_results_for_skip_accounting() -> None:
    contract = (ROOT / "tests/CMakeLists.txt").read_text()
    assert "horo_register_catch_test(HoroMetalGraphExecutionSmoke" in contract
    assert "add_test(NAME HoroMetalGraphExecutionSmoke " not in contract
    assert bundle.re.search(bundle.SELECTION, "HoroMetalGraphExecutionSmoke::Cooked native Metal UI")
    assert bundle.re.search(bundle.SELECTION, "HoroMetalGraphExecutionSmoke") is None


@pytest.mark.parametrize("field", ["GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT"])
@pytest.mark.parametrize("value", ["١", "1٢"])
def test_non_ascii_run_identity_fails_before_event_read(tmp_path: Path, monkeypatch, field: str, value: str) -> None:
    monkeypatch.setattr(bundle, "source_identity", lambda _workspace: (IDENTITY["commit"], IDENTITY["tree"]))
    monkeypatch.setenv("GITHUB_REPOSITORY", IDENTITY["repository"])
    monkeypatch.setenv("GITHUB_SHA", IDENTITY["commit"])
    monkeypatch.setenv("GITHUB_RUN_ID", "7")
    monkeypatch.setenv("GITHUB_RUN_ATTEMPT", "1")
    monkeypatch.setenv(field, value)
    with pytest.raises(ValueError, match="workflow run identity"):
        bundle.context(tmp_path)


def test_private_fixture_directories_preserve_environment_scope_and_confinement(tmp_path: Path) -> None:
    build = tmp_path / "build ü"
    home = build / "home"
    temporary = build / "temporary"
    outside = tmp_path / "outside"
    bundle.private_environment_directories([f"HOME=set:{home}", f"TMPDIR={temporary}", f"UNRELATED={outside}"], build)
    assert home.is_dir()
    assert temporary.is_dir()
    assert not outside.exists()
    with pytest.raises(ValueError, match="escapes the bundle"):
        bundle.private_environment_directories([f"HOME=set:{outside}"], build)
    assert not outside.exists()


def manifest(data: bytes) -> dict:
    return {"schema": bundle.SCHEMA, "identity": IDENTITY, "selection": bundle.SELECTION,
            "files": {MEMBER: {"size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "executable": True}}}


def archive_at(path: Path, document: dict, entries: list) -> Path:
    with tarfile.open(path, "w:gz", format=tarfile.USTAR_FORMAT) as archive:
        for name, data, kind in [(bundle.MANIFEST, json.dumps(document).encode(), tarfile.REGTYPE), *entries]:
            info = tarfile.TarInfo(name)
            info.type, info.size = kind, len(data)
            archive.addfile(info, io.BytesIO(data))
    return path


@pytest.fixture
def native_producer(tmp_path: Path, monkeypatch):
    workspace = tmp_path.resolve() / "checkout with spaces ü"
    build = workspace / bundle.BUILD
    tests = []
    for name in sorted(bundle.EXECUTABLES):
        relative = "apps/HoroEditor.app/Contents/MacOS/HoroEditor" if name == "HoroEditor" else "tests/" + name
        binary = build / relative
        binary.parent.mkdir(parents=True, exist_ok=True)
        binary.write_bytes(native_binary())
        binary.chmod(0o755)
        case_name = "HoroEditorMetalFirstFrame" if name == "HoroEditor" else name
        if name not in {"HoroEditor", "HoroMetalShaderPreparationSmoke"}:
            case_name += "::required"
        tests.append({"name": case_name,
                      "command": [str(binary)],
                      "properties": [{"name": "WORKING_DIRECTORY", "value": str(build / "tests")}]})
    (build / "tests/MetalPreparationFixture.metallib").write_bytes(b"shader fixture")
    for name in bundle.UI_PAINT_FILES:
        path = build / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"UI qualification data")
    sdk_directory = build / "sdk/gameplay"
    sdk_directory.mkdir(parents=True)
    (sdk_directory / "fixture.cmake").write_text("# runtime SDK fixture\n")
    monkeypatch.setattr(bundle, "context", lambda _workspace: IDENTITY)
    monkeypatch.setattr(bundle, "discovery", lambda _build: tests)
    return workspace, workspace / "build/metal-test-bundle" / bundle.PAYLOAD, tests


def test_package_output_is_private_fixed_and_preserves_verified_payload(native_producer, tmp_path: Path) -> None:
    workspace, output, tests = native_producer
    report = bundle.pack(workspace)
    assert report["testCount"] == len(tests)
    assert report["sha256"] == hashlib.sha256(output.read_bytes()).hexdigest()
    assert output.parent.stat().st_mode & 0o777 == 0o700
    staging = tmp_path / "consumer"
    staging.mkdir()
    document = bundle.unpack(output, staging, IDENTITY)
    bundle.validate_test_manifest(document, staging)
    assert document["tests"] == tests
    assert (staging / MEMBER).read_bytes() == native_binary()
    assert (staging / "tests/MetalPreparationFixture.metallib").read_bytes() == b"shader fixture"
    assert (staging / "sdk/gameplay/fixture.cmake").read_text() == "# runtime SDK fixture\n"
    assert all((staging / name).read_bytes() == b"UI qualification data" for name in bundle.UI_PAINT_FILES)


@pytest.mark.parametrize("name", sorted(bundle.UI_PAINT_FILES))
def test_producer_requires_every_ui_qualification_input(native_producer, name: str) -> None:
    workspace, output, _ = native_producer
    (workspace / bundle.BUILD / name).unlink()
    with pytest.raises(ValueError, match="runtime input is absent"):
        bundle.pack(workspace)
    assert not output.exists()


@pytest.mark.parametrize("name", ["extra.metallib", "PaintVertex.air", "nested/PaintVertex.metallib"])
def test_ui_payload_does_not_admit_unlisted_runtime_inputs(name: str) -> None:
    with pytest.raises(ValueError, match="undeclared file"):
        bundle.payload_path(bundle.UI_PAINT_FIXTURE + "/" + name)


@pytest.mark.parametrize("attack", ["outside", "relative", "traversal", "different-name"])
def test_cli_rejects_output_selection_before_source_inspection(native_producer, tmp_path: Path, monkeypatch,
                                                             attack: str) -> None:
    _workspace, output, _ = native_producer
    requested = {"outside": tmp_path / "outside.tar.gz", "relative": Path("build/metal-test-bundle/metal-tests.tar.gz"),
                 "traversal": output.parent / ".." / "metal-test-bundle" / bundle.PAYLOAD,
                 "different-name": output.with_name("other.tar.gz")}[attack]
    monkeypatch.setattr(bundle, "context", lambda _workspace: pytest.fail("Invalid output reached source inspection"))
    monkeypatch.setattr(sys, "argv", ["metal_test_bundle.py", "pack", "--output", str(requested)])
    with pytest.raises(SystemExit) as error:
        bundle.main()
    assert error.value.code == 2
    assert not output.parent.exists()
    assert not (tmp_path / "outside.tar.gz").exists()


@pytest.mark.parametrize("attack", ["parent-link", "file-link", "existing-file", "public-directory"])
def test_package_destination_never_replaces_files_or_follows_links(native_producer, tmp_path: Path, attack: str) -> None:
    workspace, output, _ = native_producer
    outside = tmp_path / "outside"
    outside.mkdir()
    sentinel = outside / bundle.PAYLOAD
    sentinel.write_bytes(b"preserve outside bytes")
    if attack == "parent-link":
        output.parent.symlink_to(outside, target_is_directory=True)
    else:
        output.parent.mkdir(mode=0o700)
        if attack == "file-link":
            output.symlink_to(sentinel)
        elif attack == "existing-file":
            output.write_bytes(b"preserve existing bytes")
        else:
            output.parent.chmod(0o755)
    with pytest.raises(ValueError):
        bundle.pack(workspace)
    assert sentinel.read_bytes() == b"preserve outside bytes"
    if attack == "existing-file":
        assert output.read_bytes() == b"preserve existing bytes"


def test_exclusive_bundle_creation_preserves_a_file_created_after_validation(native_producer, monkeypatch) -> None:
    workspace, output, _ = native_producer

    def inspect(_workspace):
        output.write_bytes(b"created after validation")
        return IDENTITY

    monkeypatch.setattr(bundle, "context", inspect)
    with pytest.raises(FileExistsError):
        bundle.pack(workspace)
    assert output.read_bytes() == b"created after validation"


@pytest.mark.parametrize("schema", [1, 3, True, "2", None])
def test_bundle_schema_requires_the_current_relocatable_path_contract(schema) -> None:
    document = manifest(native_binary())
    document["schema"] = schema
    encoded = json.dumps(document).encode()
    with pytest.raises(ValueError, match="bundle identity"):
        bundle.validated_manifest(encoded, IDENTITY)


@pytest.mark.parametrize("roots", [[], ["/old/source/build/ci-metal", "/old/physical/build/ci-metal"],
                                  "/producer/build/ci-metal", None])
def test_bundle_rejects_legacy_or_ambiguous_root_contracts(roots) -> None:
    with pytest.raises(ValueError, match="producer path contract"):
        bundle.validate_producer_roots(roots)


def test_shader_fixture_path_contract_uses_its_explicit_ctest_working_directory() -> None:
    cmake = (ROOT / "tests/CMakeLists.txt").read_text()
    start = cmake.index("target_compile_definitions(HoroMetalShaderPreparationSmoke")
    end = cmake.index("    endif ()", start)
    contract = cmake[start:end]
    assert 'HORO_METAL_PREPARATION_FIXTURE="MetalPreparationFixture.metallib"' in contract
    assert 'WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"' in contract
    assert 'TIMEOUT 60 RUN_SERIAL TRUE SKIP_RETURN_CODE 4' in contract


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
    assert actual["name"] == name
    assert actual["command"] == test["command"]
    properties = {item["name"]: item["value"] for item in actual["properties"]}
    assert properties["RESOURCE_LOCK"] == ["original-lock"]
    assert properties["ENVIRONMENT"] == ["KEY=literal;semicolon"]
    assert properties["RUN_SERIAL"] is True
    assert properties["SKIP_RETURN_CODE"] == 4
    assert properties["TIMEOUT"] == 60


@pytest.mark.parametrize("field", list(IDENTITY))
def test_retained_producer_bundle_cannot_change_any_bound_source_field(field: str) -> None:
    document = manifest(native_binary())
    document["identity"] = dict(IDENTITY)
    document["identity"][field] = 9 if field in {"attempt", "run"} else "d" * 40
    consumer = dict(IDENTITY, attempt=2)
    expected = artifact.producer_identity(consumer, "1")
    encoded = json.dumps(document).encode()
    with pytest.raises(ValueError, match="bundle identity"):
        bundle.validated_manifest(encoded, expected)


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
    document = {"tests": [], "producerRoots": [root]}
    with pytest.raises(ValueError, match="producer path contract"):
        bundle.validate_test_manifest(document, tmp_path)
