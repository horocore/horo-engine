"""The Extension Release route must execute both complete, unsampled allocation probes."""

from html import escape
import importlib.util
from pathlib import Path
import re

import pytest

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
        output = "\n".join(f"[{marker}] measured={index + 1} completed={index + 1}" for index in range(count))
        bodies.append(f'<testcase name="{escape(name, quote=True)}"><system-out>{output}</system-out></testcase>')
    path = tmp_path / "result.xml"
    path.write_text(transform('<testsuite failures="0" errors="0" skipped="0">' + "".join(bodies) + '</testsuite>'), encoding="utf-8")
    return path


def test_both_exact_cases_and_all_four_sweeps_are_accepted(tmp_path):
    QUALIFICATION.verify_discovery(discovery())
    QUALIFICATION.verify_junit(junit(tmp_path))


@pytest.mark.parametrize("alteration", ["missing", "duplicate", "disabled", "unexpected"])
def test_discovery_requires_both_enabled_exact_cases(alteration):
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
    assert "--target" in build and "HoroExtensionManagerTests" in build
    step = re.search(r"- name: Test Windows Release extension allocation qualification\n(.*?)(?=\n      - name:)", workflow, re.S).group(1)
    for required in ("steps.mcp_release_build.outcome == 'success'", "--show-only=json-v1", "--discovery", "--output-junit",
                     "--no-tests=error --timeout 90", "run_extension_allocation_qualification.py"):
        assert required in step
    assert "continue-on-error" not in step
    assert "            build/ci-audio-release/extension-allocation-ctest.xml" in workflow
    cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    assert 'if(NOT MSVC OR CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")' in cmake
    assert 'COMMAND HoroExtensionManagerTests "${extension_allocation_case}"' in cmake
    for name in QUALIFICATION.CASES:
        assert name.removeprefix(QUALIFICATION.PREFIX) in cmake
    for filename, case in (("EditorActivityRecoveryTests.cpp", next(iter(QUALIFICATION.CASES))),
                           ("ExtensionManifestParserTests.cpp", list(QUALIFICATION.CASES)[1])):
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
