"""Require exhaustive Navigation fault sweeps in the existing Windows Release route."""

import importlib.util
from html import escape
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "navigation_allocation_route", ROOT / "scripts/ci/run_scene_allocation_qualification.py"
)
ROUTE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ROUTE)


@pytest.fixture(autouse=True)
def evidence_workspace(tmp_path, monkeypatch):
    """Keep exact discovery and execution receipts inside their authorized workspace."""
    monkeypatch.chdir(tmp_path)


def report(tmp_path, marker="complete", dropped=None, status="run"):
    """Create explicit evidence without invoking a compiler or a fake production adapter."""
    records = []
    for name, phase in ROUTE.NAVIGATION_PHASES.items():
        if name == dropped:
            continue
        output = f"[navigation-allocation] {phase} measured=8946 completed=8946"
        if marker == "partial":
            output = output.replace("completed=8946", "completed=4096")
        elif marker == "zero":
            output = output.replace("8946", "0")
        elif marker == "absent":
            output = ""
        records.append(f'<testcase name="{escape(name, quote=True)}" status="{status}">'
                       f'<system-out>{output}</system-out></testcase>')
    path = tmp_path / "navigation.xml"
    path.write_text(
        '<testsuite failures="0" errors="0" skipped="0">' + "".join(records) + '</testsuite>', encoding="utf-8"
    )
    return path


def test_all_three_cases_and_every_measured_position_qualify(tmp_path):
    assert len(ROUTE.NAVIGATION_CASES) == 3
    discovery = {"tests": [{"name": name, "properties": []} for name in ROUTE.NAVIGATION_CASES]}
    ROUTE.verify_discovery(discovery, ROUTE.NAVIGATION_CASES)
    ROUTE.verify_junit(report(tmp_path), ROUTE.NAVIGATION_CASES)
    pattern = tmp_path / "navigation-regex.txt"
    ROUTE.write_regex(pattern, ROUTE.NAVIGATION_CASES)
    assert pattern.read_text().startswith("^(")
    assert pattern.read_text().endswith(")$")


@pytest.mark.parametrize("marker", ["partial", "zero", "absent"])
def test_sampled_or_unexercised_fault_sweeps_fail_closed(tmp_path, marker):
    receipt = report(tmp_path, marker)
    with pytest.raises(ValueError, match="positions incomplete"):
        ROUTE.verify_junit(receipt, ROUTE.NAVIGATION_CASES)


@pytest.mark.parametrize("status", ["notrun", "disabled"])
def test_unexecuted_cases_cannot_qualify(tmp_path, status):
    receipt = report(tmp_path, status=status)
    with pytest.raises(ValueError, match="did not pass"):
        ROUTE.verify_junit(receipt, ROUTE.NAVIGATION_CASES)


def test_missing_discovery_or_execution_cannot_replace_a_fault_case(tmp_path):
    with pytest.raises(ValueError, match="discovery differs"):
        ROUTE.verify_discovery({"tests": []}, ROUTE.NAVIGATION_CASES)
    receipt = report(tmp_path, dropped=next(iter(ROUTE.NAVIGATION_CASES)))
    with pytest.raises(ValueError, match="execution differs"):
        ROUTE.verify_junit(receipt, ROUTE.NAVIGATION_CASES)


def test_route_builds_same_release_target_and_verifies_actual_discovery_and_junit():
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    build = workflow.split("id: mcp_release_build", 1)[1].split("\n      - name:", 1)[0]
    assert "--preset ci-audio-release" in build
    assert "HoroNavigationProjectSettingsTests" in build
    block = workflow.split("- name: Test Windows Release Navigation settings allocation qualification", 1)[1]
    block = block.split("\n      - name:", 1)[0]
    for required in ("!cancelled()", "steps.mcp_release_build.outcome == 'success'", "--show-only=json-v1",
                     "--family navigation --discovery", "--no-tests=error", "--parallel 1", "--timeout 90",
                     "--family navigation --output-junit", '--output-junit "navigation-allocation-ctest.xml"'):
        assert required in block
    assert "debug" not in block.lower()


def test_whole_job_deadlines_leave_individual_fault_case_budget_unchanged():
    for name, job in (("ci.yml", "test"), ("sonar.yml", "sonar")):
        workflow = (ROOT / ".github/workflows" / name).read_text(encoding="utf-8")
        block = workflow.split(f"\n  {job}:\n", 1)[1]
        assert "    timeout-minutes: 120" in block
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    block = workflow.split("- name: Test Windows Release Navigation settings allocation qualification", 1)[1]
    block = block.split("\n      - name:", 1)[0]
    assert "--timeout 90" in block
    assert "--timeout 120" not in block
