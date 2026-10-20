
import sys
"""Collision authoring keeps canonical authority and mandatory Release fault evidence."""

import importlib.util
from html import escape
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "collision_allocation_qualification", ROOT / 'scripts/dev.py'
)
QUALIFICATION = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = QUALIFICATION
SPEC.loader.exec_module(QUALIFICATION)


def report(tmp_path, completed=8946, status="run", omitted=False):
    cases = []
    for name in sorted(QUALIFICATION.COLLISION_CASES):
        sidecar = name == QUALIFICATION.SIDECAR_CASE
        phases = QUALIFICATION.SIDECAR_PHASES if sidecar else (QUALIFICATION.COLLISION_PHASES[name],)
        prefix = "collision-sidecar-allocation" if sidecar else "collision-allocation"
        proof = "" if omitted else "\n".join(
            f"[{prefix}] {phase} measured=8946 completed={completed}" for phase in phases
        )
        cases.append(
            f'<testcase name="{escape(name, quote=True)}" status="{status}">'
            f"<system-out>{proof}</system-out></testcase>"
        )
    path = tmp_path / "collision.xml"
    path.write_text('<testsuite failures="0" errors="0" skipped="0">' +
                    "".join(cases) + "</testsuite>", encoding="utf-8")
    return path


def test_exact_release_cases_and_every_measured_position(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    cases = QUALIFICATION.COLLISION_CASES
    assert len(cases) == 5
    assert QUALIFICATION.SIDECAR_CASE in cases
    assert len(QUALIFICATION.SIDECAR_PHASES) == 4
    assert "HoroProjectCollisionSettingsTests::Native registry inventory recovers after every one-shot allocation failure" in cases
    QUALIFICATION.scene_verify_discovery({"tests": [
        {"name": name, "properties": []} for name in cases
    ]}, cases)
    QUALIFICATION.verify_collision_positions(report(tmp_path))


@pytest.mark.parametrize("completed,status,omitted", [
    (8945, "run", False), (0, "run", False), (8946, "notrun", False),
    (8946, "run", True),
])
def test_partial_absent_or_unexecuted_sweep_never_qualifies(
        tmp_path, monkeypatch, completed, status, omitted):
    monkeypatch.chdir(tmp_path)
    path = report(tmp_path, completed, status, omitted)
    with pytest.raises(ValueError):
        QUALIFICATION.verify_collision_positions(path)


@pytest.mark.parametrize("phase", QUALIFICATION.SIDECAR_PHASES)
@pytest.mark.parametrize("fault", ("missing", "zero", "partial", "duplicate"))
def test_every_direct_decoder_sweep_requires_one_positive_complete_marker(tmp_path, monkeypatch, phase, fault):
    monkeypatch.chdir(tmp_path)
    path = report(tmp_path)
    marker = f"[collision-sidecar-allocation] {phase} measured=8946 completed=8946"
    replacements = {
        "missing": "",
        "zero": f"[collision-sidecar-allocation] {phase} measured=0 completed=0",
        "partial": f"[collision-sidecar-allocation] {phase} measured=8946 completed=8945",
        "duplicate": marker + "\n" + marker,
    }
    path.write_text(path.read_text().replace(marker, replacements[fault]), encoding="utf-8")
    with pytest.raises(ValueError):
        QUALIFICATION.verify_collision_positions(path)


def test_release_route_builds_real_target_and_retains_exact_results():
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    build = workflow.split("- name: Build Windows Release allocation qualification", 1)[1]
    assert "HoroProjectCollisionSettingsTests" in build.split("\n      - name:", 1)[0]
    block = workflow.split("- name: Test Windows Release Collision settings allocation qualification", 1)[1]
    block = block.split("\n      - name:", 1)[0]
    for required in ("!cancelled()", "steps.mcp_release_build.outcome == 'success'",
                     "--show-only=json-v1", "--family collision", "--parallel 1",
                     "--no-tests=error", "--timeout 90", "--test-output-size-passed 16384",
                     '--output-junit "collision-allocation-ctest.xml"'):
        assert required in block
    assert "debug" not in block.lower()
    assert workflow.count("build/ci-audio-release/collision-allocation-ctest.xml") == 1


def test_collision_copy_is_localized_and_both_gui_cases_are_coverage_targets():
    import json

    catalogs = [json.loads((ROOT / "assets/localization/editor" / name).read_text(
        encoding="utf-8"))["messages"] for name in ("en-US.json", "tr-TR.json")]
    keys = [{key for key in catalog if key.startswith("settings.collision.")}
            for catalog in catalogs]
    assert keys[0] == keys[1]
    assert len(keys[0]) >= 30
    suites = (ROOT / "tests/cmake/HoroCiSuites.cmake").read_text(encoding="utf-8")
    coverage = suites.split("set(HORO_SONAR_EDITOR_TARGETS", 1)[1].split(")", 1)[0]
    assert "HoroSettingsModalLifecycleTests" in coverage
    assert "HoroEditorWorkspaceControllerTests" in coverage


def test_extended_job_budget_does_not_relax_case_timeout():
    for path in ("ci.yml", "sonar.yml"):
        workflow = (ROOT / ".github/workflows" / path).read_text(encoding="utf-8")
        assert "    timeout-minutes: 180" in workflow
