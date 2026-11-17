"""Unit coverage of JUnit admission; fixture bytes do not represent a native execution."""
from pathlib import Path
import importlib.util
import subprocess

import pytest
from defusedxml.common import DefusedXmlException


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests/integration/physics/run_physics_state_restart.py"


@pytest.fixture
def restart_report(tmp_path, monkeypatch):
    spec = importlib.util.spec_from_file_location("physics_restart_report_under_test", SCRIPT)
    assert spec is not None and spec.loader is not None
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    binary = tmp_path / "child-fixture"
    binary.write_bytes(b"report parser unit fixture")

    def successful_child(command, **_kwargs):
        return subprocess.CompletedProcess(command, 0)

    monkeypatch.setattr(runner.subprocess, "run", successful_child)
    return runner, binary, tmp_path


def test_restart_report_accepts_a_passing_junit_case(restart_report):
    runner, binary, directory = restart_report
    (directory / "capture.xml").write_text(
        '<testsuites><testsuite><testcase name="owned"/></testsuite></testsuites>', encoding="utf-8")
    runner.run_child(binary, directory, "capture", runner.digest(binary))


@pytest.mark.parametrize("child", [
    "",
    '<testcase name="owned"><skipped/></testcase>',
    '<testcase name="owned"><failure/></testcase>',
    '<testcase name="owned"><error/></testcase>',
])
def test_restart_report_rejects_missing_or_nonpassing_cases(restart_report, child):
    runner, binary, directory = restart_report
    (directory / "capture.xml").write_text(f"<testsuite>{child}</testsuite>", encoding="utf-8")
    with pytest.raises(RuntimeError, match="real non-skipped passing native child"):
        runner.run_child(binary, directory, "capture", runner.digest(binary))


@pytest.mark.parametrize("declaration", [
    '<!ENTITY sample "unexpected">',
    '<!ENTITY sample SYSTEM "file:///must-not-read">',
])
def test_restart_report_rejects_dtd_and_entity_payloads(restart_report, declaration):
    runner, binary, directory = restart_report
    (directory / "capture.xml").write_text(
        f'<!DOCTYPE testsuite [{declaration}]><testsuite><testcase name="&sample;"/></testsuite>', encoding="utf-8")
    with pytest.raises(DefusedXmlException):
        runner.run_child(binary, directory, "capture", runner.digest(binary))
