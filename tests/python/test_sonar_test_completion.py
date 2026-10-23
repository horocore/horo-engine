"""Exercise coverage publication safety with actual CTest reports."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import shutil
# Subprocesses execute only the fixed CMake/CTest fixture commands below.
import subprocess  # nosec B404

import pytest
from defusedxml.common import DefusedXmlException


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "sonar_completion", ROOT / ".github/scripts/validate_sonar_test_completion.py"
)
assert SPEC is not None and SPEC.loader is not None
completion = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(completion)


@pytest.fixture
def reports(tmp_path: Path) -> tuple[Path, Path]:
    cmake = shutil.which("cmake")
    ctest = shutil.which("ctest")
    if not cmake or not ctest:
        pytest.skip("CMake and CTest required for report fixture")
    (tmp_path / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.21)
project(Completion NONE)
enable_testing()
add_test(NAME passed COMMAND "${CMAKE_COMMAND}" -E true)
add_test(NAME failed COMMAND "${CMAKE_COMMAND}" -E false)
add_test(NAME disabled COMMAND "${CMAKE_COMMAND}" -E true)
set_tests_properties(disabled PROPERTIES DISABLED TRUE)
add_test(NAME skipped COMMAND "${CMAKE_COMMAND}" -E echo SKIP)
set_tests_properties(skipped PROPERTIES SKIP_REGULAR_EXPRESSION SKIP)
add_test(NAME excluded COMMAND "${CMAKE_COMMAND}" -E false)
set_tests_properties(passed failed disabled skipped PROPERTIES LABELS coverage)
''', encoding="utf-8")
    build = tmp_path / "build"
    # executable selects the resolved absolute binary; argv[0] is only its name.
    # No shell, user command, or PATH search occurs in these subprocess calls.
    subprocess.run(  # nosec B607
        ["cmake", "-S", str(tmp_path), "-B", str(build)],
        executable=str(Path(cmake).resolve(strict=True)),
        shell=False, check=True, capture_output=True,  # nosec B603
    )
    discovered = subprocess.run(  # nosec B607
        ["ctest", "--test-dir", str(build), "-L", "^coverage$", "--show-only=json-v1"],
        executable=str(Path(ctest).resolve(strict=True)),
        shell=False, check=True, capture_output=True,  # nosec B603
    )
    discovery = tmp_path / "discovery.json"
    discovery.write_bytes(discovered.stdout)
    junit = tmp_path / "ctest.xml"
    result = subprocess.run(  # nosec B607
        ["ctest", "--test-dir", str(build), "-L", "^coverage$", "--output-junit", str(junit)],
        executable=str(Path(ctest).resolve(strict=True)),
        shell=False, capture_output=True,  # nosec B603
    )
    assert result.returncode == 8, result.stdout
    return discovery, junit


def test_complete_failed_suite_is_evidence_not_a_pass(reports: tuple[Path, Path]) -> None:
    discovery, junit = reports
    assert set(completion.selected_tests(discovery)) == {"passed", "failed", "disabled", "skipped"}
    completion.validate_completion(discovery, junit)


@pytest.mark.parametrize("replacement", [
    '<testcase name="passed" status="run"/>',
    '<testcase name="passed" status="notrun"/>',
    '<testcase name="passed" status="disabled"/>',
    '<testcase name="unexpected" status="run"/>',
    '<testcase name="passed" status="fail"/>',
    '<testcase name="passed" status="run"><failure/></testcase>',
    '<testcase name="passed" status="run"/><testcase name="passed" status="run"/>',
])
def test_partial_or_inconsistent_report_rejected(
    tmp_path: Path, replacement: str,
) -> None:
    discovery = tmp_path / "discovery.json"
    discovery.write_text(json.dumps({"kind": "ctestInfo", "tests": [
        {"name": "passed"}, {"name": "other"},
    ]}))
    junit = tmp_path / "ctest.xml"
    junit.write_text(f'<testsuite tests="2">{replacement}</testsuite>')
    with pytest.raises(ValueError):
        completion.validate_completion(discovery, junit)


@pytest.mark.parametrize("case", [
    '<testcase name="passed" status="notrun"/>',
    '<testcase name="passed" status="disabled"/>',
    '<testcase name="passed" status="fail"/>',
    '<testcase name="passed" status="run"><failure/></testcase>',
])
def test_missing_terminal_status_rejected(tmp_path: Path, case: str) -> None:
    discovery = tmp_path / "discovery.json"
    discovery.write_text(json.dumps({"kind": "ctestInfo", "tests": [{"name": "passed"}]}))
    junit = tmp_path / "ctest.xml"
    junit.write_text(f'<testsuite tests="1">{case}</testsuite>')
    with pytest.raises(ValueError, match="terminal result"):
        completion.validate_completion(discovery, junit)


@pytest.mark.parametrize("tests", [[], [{"name": "same"}, {"name": "same"}]])
def test_empty_or_duplicate_discovery_rejected(tmp_path: Path, tests: list) -> None:
    discovery = tmp_path / "discovery.json"
    discovery.write_text(json.dumps({"kind": "ctestInfo", "tests": tests}))
    with pytest.raises(ValueError):
        completion.selected_tests(discovery)


def test_entity_expansion_and_oversized_reports_rejected(tmp_path: Path) -> None:
    discovery = tmp_path / "discovery.json"
    discovery.write_text(json.dumps({"kind": "ctestInfo", "tests": [{"name": "passed"}]}))
    junit = tmp_path / "ctest.xml"
    junit.write_text('<!DOCTYPE x [<!ENTITY x "data">]><testsuite tests="1">&x;</testsuite>')
    with pytest.raises(DefusedXmlException):
        completion.validate_completion(discovery, junit)
    with pytest.raises(ValueError, match="size limit"):
        completion.read_bounded(junit, 8)


def test_workflow_keeps_test_failures_and_pr_fail_fast() -> None:
    workflow = (ROOT / ".github/workflows/sonar.yml").read_text(encoding="utf-8")
    steps = {
        block.splitlines()[0]: block
        for block in workflow.split("      - name: ")[1:]
    }
    cpp = steps["Run C++ coverage tests"]
    python = steps["Run Python coverage tests"]
    assert "ctest --preset sonar --show-only=json-v1" in cpp
    assert "xvfb-run -a ctest --preset sonar" in cpp
    assert "steps.build.outcome == 'success'" in cpp
    assert "(github.ref == 'refs/heads/main' && steps.cpp_tests.outcome == 'failure')" in python
    for name in ("Run C++ coverage tests", "Run Python coverage tests",
                 "Collect fastcov line coverage", "Analyze code and wait for Quality Gate"):
        assert "continue-on-error" not in steps[name]
        assert "!cancelled()" in steps[name]
    collector = steps["Collect fastcov line coverage"]
    assert "steps.python_tests.outcome == 'success'" in collector
    assert collector.index("validate_sonar_test_completion.py") < collector.index("collect_sonar_coverage.sh")
    assert "steps.coverage.outcome == 'success'" in steps["Install cached Sonar scanner"]
    assert "steps.scanner_install.outcome == 'success'" in steps["Analyze code and wait for Quality Gate"]
    assert "-Dsonar.qualitygate.wait=true" in workflow
