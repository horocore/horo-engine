"""The Extension/Workspace Release route must execute every complete, unsampled allocation probe."""

from argparse import ArgumentTypeError
from html import escape
import importlib.util
from pathlib import Path
import re
import json
import subprocess
import sys

import pytest

from scripts.ci import allocation_qualification_cli as EVIDENCE

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "extension_allocation_qualification", ROOT / "scripts/ci/run_extension_allocation_qualification.py"
)
QUALIFICATION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(QUALIFICATION)


def discovery():
    return {"tests": [{"name": name, "properties": []} for name in QUALIFICATION.CASES]}


def junit(tmp_path, transform=lambda body: body):
    bodies = []
    for name, (marker, count) in QUALIFICATION.CASES.items():
        if marker == "workspace-allocation":
            output = f"[{marker}] measured=7\n[{marker}] completed=7"
        else:
            output = "\n".join(f"[{marker}] measured={index + 1} completed={index + 1}" for index in range(count))
        bodies.append(f'<testcase name="{escape(name, quote=True)}"><system-out>{output}</system-out></testcase>')
    path = tmp_path / "result.xml"
    path.write_text(transform('<testsuite failures="0" errors="0" skipped="0">' + "".join(bodies) + '</testsuite>'), encoding="utf-8")
    return path


def test_all_exact_cases_and_all_five_sweeps_are_accepted(tmp_path):
    QUALIFICATION.verify_discovery(discovery())
    QUALIFICATION.verify_junit(junit(tmp_path))


@pytest.mark.parametrize("alteration", ["missing", "duplicate", "disabled", "unexpected"])
def test_discovery_requires_all_enabled_exact_cases(alteration):
    document = discovery()
    if alteration == "missing":
        document["tests"].pop()
    elif alteration == "duplicate":
        document["tests"].append(document["tests"][0])
    elif alteration == "disabled":
        document["tests"][0]["properties"] = [{"name": "DISABLED", "value": True}]
    else:
        document["tests"].append({"name": QUALIFICATION.PREFIX + "unexpected"})
    with pytest.raises(ValueError):
        QUALIFICATION.verify_discovery(document)


@pytest.mark.parametrize("replacement", ["", "[manifest-allocation] measured=0 completed=0",
                                          "[manifest-allocation] measured=1 completed=0"])
def test_missing_zero_or_partial_sweep_cannot_pass(tmp_path, replacement):
    path = junit(tmp_path, lambda body: body.replace("[manifest-allocation] measured=1 completed=1", replacement))
    with pytest.raises(ValueError):
        QUALIFICATION.verify_junit(path)


@pytest.mark.parametrize("tag", ["failure", "error", "skipped"])
def test_nonpassing_execution_is_rejected(tmp_path, tag):
    path = junit(tmp_path, lambda body: body.replace("<system-out>", f"<{tag}/><system-out>", 1))
    with pytest.raises(ValueError):
        QUALIFICATION.verify_junit(path)


def test_checked_debug_keeps_functional_tests_and_release_runs_full_probes():
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    build = re.search(r"id: mcp_release_build\n(.*?)(?=\n      - name:)", workflow, re.S).group(1)
    assert "--target" in build
    assert "HoroExtensionManagerTests" in build
    assert "HoroWorkspaceLayoutPersistenceTests" in build
    step = re.search(r"- name: Test Windows Release extension allocation qualification\n(.*?)(?=\n      - name:)", workflow, re.S).group(1)
    for required in ("steps.mcp_release_build.outcome == 'success'", "--show-only=json-v1", "--discovery", "--output-junit",
                     "--no-tests=error --timeout 90", "--test-output-size-passed 65536", "run_extension_allocation_qualification.py"):
        assert required in step
    assert "continue-on-error" not in step
    assert "            build/ci-audio-release/extension-allocation-ctest.xml" in workflow
    cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    assert 'if(NOT MSVC OR CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")' in cmake
    assert 'COMMAND HoroExtensionManagerTests "${extension_allocation_case}"' in cmake
    for name in QUALIFICATION.CASES:
        if name.startswith(QUALIFICATION.PREFIX):
            assert name.removeprefix(QUALIFICATION.PREFIX) in cmake
    assert "HoroWorkspaceLayoutPersistenceTests" in cmake
    assert "HoroWorkspaceLayoutAllocationTests" not in cmake
    extension_cases = [name for name in QUALIFICATION.CASES if name.startswith(QUALIFICATION.PREFIX)]
    for filename, case in (("EditorActivityRecoveryTests.cpp", extension_cases[0]),
                           ("ExtensionManifestParserTests.cpp", extension_cases[1])):
        source = (ROOT / "tests/unit/extensions" / filename).read_text(encoding="utf-8")
        position = source.index(case.removeprefix(QUALIFICATION.PREFIX))
        guard = source.rfind("#if !defined(_MSC_VER) || _ITERATOR_DEBUG_LEVEL == 0", 0, position)
        assert guard >= 0
        assert "#endif" in source[source.index(case.removeprefix(QUALIFICATION.PREFIX)):]


def test_workspace_provider_recovery_is_in_actual_windows_build_and_test_routes():
    suites = (ROOT / "tests/cmake/HoroCiSuites.cmake").read_text(encoding="utf-8")
    selected = re.search(r"set\(HORO_CI_WINDOWS_TARGETS\s+(.*?)\)", suites, re.S)
    assert selected
    assert {"HoroWorkspaceLayoutPersistenceTests", "HoroWorkspaceRecoveryPublicHeaderConsumer"} <= set(selected.group(1).split())
    assert "if(target IN_LIST HORO_CI_WINDOWS_TARGETS)" in suites
    assert "list(APPEND ARG_LABELS ci-windows)" in suites


@pytest.mark.parametrize("replacement", ["", "[workspace-allocation] measured=0", "[workspace-allocation] measured=8"])
def test_workspace_sweep_cannot_omit_zero_or_miscount_a_position(tmp_path, replacement):
    path = junit(tmp_path, lambda body: body.replace("[workspace-allocation] measured=7", replacement))
    with pytest.raises(ValueError):
        QUALIFICATION.verify_junit(path)


def test_split_fault_injection_is_mandatory_release_and_functional_debug_remains():
    source = (ROOT / "tests/unit/editor/WorkspaceLayoutPersistenceTests.cpp").read_text(encoding="utf-8")
    start = source.index('TEST_CASE("Split parsing releases every owned allocation')
    assert source.rfind("#if !defined(_MSC_VER) || _ITERATOR_DEBUG_LEVEL == 0", 0, start) >= 0
    next_case = source.index('TEST_CASE("Split children transfer exclusive ownership', start)
    stop = source.rfind("#endif", start, next_case)
    assert stop > start
    assert 'TEST_CASE("Split children transfer exclusive ownership' in source[stop:]
    assert "for (std::size_t index = 0; index < successfulAllocations; ++index)" in source[start:stop]
    assert "CHECK(allocations == frees + 1)" in source[start:stop]


@pytest.mark.parametrize("absolute", [False, True])
def test_shared_writer_accepts_in_workspace_spaces_and_exact_cases(tmp_path, monkeypatch, absolute):
    monkeypatch.chdir(tmp_path)
    destination = tmp_path / "regex with spaces.txt"
    QUALIFICATION.write_case_regex(destination if absolute else destination.name, QUALIFICATION.CASES)
    pattern = destination.read_text(encoding="utf-8")
    for name in QUALIFICATION.CASES:
        assert re.fullmatch(pattern, name)
    assert re.fullmatch(pattern, "unselected test") is None


@pytest.mark.parametrize("absolute", [False, True])
def test_shared_admission_and_actual_writer_reject_traversal(tmp_path, monkeypatch, absolute):
    root = tmp_path / "workspace"
    root.mkdir()
    monkeypatch.chdir(root)
    outside = tmp_path / "must not be created.txt"
    argument = outside if absolute else Path("..") / outside.name
    with pytest.raises(ArgumentTypeError):
        EVIDENCE.workspace_path(argument)
    with pytest.raises(ValueError):
        QUALIFICATION.write_case_regex(argument, QUALIFICATION.CASES)
    assert not outside.exists()


def test_shared_writer_rechecks_symlink_after_argument_admission(tmp_path, monkeypatch):
    root = tmp_path / "workspace"
    root.mkdir()
    monkeypatch.chdir(root)
    inside = root / "evidence"
    inside.mkdir()
    admitted = EVIDENCE.workspace_path(inside / "regex.txt")
    inside.rmdir()
    try:
        inside.symlink_to(tmp_path, target_is_directory=True)
    except OSError:
        pytest.skip("Host does not permit test symlink creation")
    with pytest.raises(ValueError):
        QUALIFICATION.write_case_regex(admitted, QUALIFICATION.CASES)
    assert not (tmp_path / "regex.txt").exists()


def test_shared_cli_still_requires_discovery_output(monkeypatch):
    monkeypatch.setattr("sys.argv", ["qualification", "--discovery", "discovery.json"])
    with pytest.raises(SystemExit) as error:
        QUALIFICATION.parse_evidence_arguments("qualification")
    assert error.value.code == 2


@pytest.mark.parametrize("family", ["extension", "scene", "character"])
def test_all_three_standalone_clis_keep_the_same_discovery_contract(tmp_path, family):
    script = ROOT / f"scripts/ci/run_{family}_allocation_qualification.py"
    spec = importlib.util.spec_from_file_location(f"{family}_cli_contract", script)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    selected = {"tests": [{"name": name, "properties": []} for name in module.CASES]}
    evidence = tmp_path / "discovery with spaces.json"
    evidence.write_text(json.dumps(selected), encoding="utf-8")
    output = tmp_path / "exact regex.txt"
    result = subprocess.run([sys.executable, str(script), "--discovery", str(evidence), "--regex-output", str(output)],
                            cwd=tmp_path, capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stderr
    pattern = output.read_text(encoding="utf-8")
    for name in module.CASES:
        assert re.fullmatch(pattern, name)
    assert re.fullmatch(pattern, "unexpected case") is None
