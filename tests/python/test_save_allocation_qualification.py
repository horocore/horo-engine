"""Positive qualification requires both recovered Save generator branches."""
from __future__ import annotations

import sys

import importlib.util
from pathlib import Path
from html import escape

import defusedxml.ElementTree as etree
from defusedxml.common import DefusedXmlException

import pytest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "save_qualification", ROOT / 'scripts/dev.py')
assert SPEC
assert SPEC.loader
QUALIFICATION = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = QUALIFICATION
SPEC.loader.exec_module(QUALIFICATION)


@pytest.fixture(autouse=True)
def explicit_workspace(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """Each direct parser caller owns an explicit workspace, like the real CI composition."""
    monkeypatch.chdir(tmp_path)


def report(tmp_path: Path, *, omitted: str = "", outcome: str = "") -> Path:
    cases = []
    for name, phase in QUALIFICATION.SAVE_CASES.items():
        output = []
        for variant in ("success", "failure"):
            markers = (f"{phase}-sync-{variant}", f"{phase}-failure-reset",
                       f"{phase}-reopen-returned", f"{phase}-reconcile-returned",
                       f"{phase}-fixture-cleanup-next", "fixture-scope-exited")
            output.extend(f"[save-allocation] {marker}\n" for marker in markers if marker != omitted)
        result = f"<{outcome}/>" if outcome else ""
        cases.append(f'<testcase name="{escape(QUALIFICATION.SAVE_PREFIX + name, quote=True)}" status="run">'
                     f"{result}<system-out>{escape(''.join(output))}</system-out></testcase>")
    path = tmp_path / "qualification.xml"
    path.write_text('<testsuite tests="2" failures="0" skipped="0">' + "".join(cases) + "</testsuite>", encoding="utf-8")
    return path


def test_both_variants_and_completed_cleanup_qualify(tmp_path: Path) -> None:
    QUALIFICATION.verify_report(report(tmp_path))


@pytest.mark.parametrize("outcome", ["skipped", "failure", "error"])
def test_nonpassing_case_cannot_qualify(tmp_path: Path, outcome: str) -> None:
    path = report(tmp_path, outcome=outcome)
    with pytest.raises(ValueError, match="did not pass"):
        QUALIFICATION.verify_report(path)


@pytest.mark.parametrize("omitted", ["lifecycle-sync-failure", "retention-sync-success",
                                     "lifecycle-reopen-returned", "retention-reconcile-returned",
                                     "fixture-scope-exited"])
def test_partial_or_unfinished_variant_cannot_qualify(tmp_path: Path, omitted: str) -> None:
    path = report(tmp_path, omitted=omitted)
    with pytest.raises(ValueError, match="Incomplete"):
        QUALIFICATION.verify_report(path)


def test_missing_case_cannot_qualify(tmp_path: Path) -> None:
    path = report(tmp_path)
    suite = etree.parse(path)
    suite.getroot().remove(suite.getroot()[0])
    suite.write(path)
    with pytest.raises(ValueError, match="exactly"):
        QUALIFICATION.verify_report(path)


@pytest.mark.parametrize("attribute,value", [("tests", "0"), ("failures", "1"), ("skipped", "1")])
def test_suite_summary_cannot_hide_missing_or_skipped_cases(tmp_path: Path, attribute: str, value: str) -> None:
    path = report(tmp_path)
    suite = etree.parse(path)
    suite.getroot().set(attribute, value)
    suite.write(path)
    with pytest.raises(ValueError, match="suite must report"):
        QUALIFICATION.verify_report(path)


def test_xml_entity_declarations_cannot_qualify(tmp_path: Path) -> None:
    path = tmp_path / "entity.xml"
    path.write_text('<!DOCTYPE testsuite [<!ENTITY injected "expanded">]><testsuite>&injected;</testsuite>', encoding="utf-8")
    with pytest.raises(DefusedXmlException):
        QUALIFICATION.verify_report(path)


@pytest.mark.parametrize("absolute", [False, True])
def test_cli_qualifies_reports_in_the_explicit_workspace(tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
                                                       capsys: pytest.CaptureFixture[str], absolute: bool) -> None:
    workspace = tmp_path / "workspace with spaces"
    workspace.mkdir()
    path = report(workspace)
    monkeypatch.chdir(workspace)
    argument = str(path) if absolute else path.name
    monkeypatch.setattr("sys.argv", ['dev.py', argument])
    QUALIFICATION.save_main()
    assert "2 passed cases, both variants completed, zero skips" in capsys.readouterr().out


@pytest.mark.parametrize("escape_kind", ["parent", "absolute", "symlink"])
def test_cli_rejects_report_paths_outside_its_workspace(tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
                                                      capsys: pytest.CaptureFixture[str], escape_kind: str) -> None:
    outside = report(tmp_path)
    workspace = tmp_path / "workspace"
    workspace.mkdir()
    if escape_kind == "symlink":
        link = workspace / "linked.xml"
        link.symlink_to(outside)
        argument = link.name
    elif escape_kind == "absolute":
        argument = str(outside)
    else:
        argument = "../qualification.xml"
    monkeypatch.chdir(workspace)
    monkeypatch.setattr("sys.argv", ['dev.py', argument])
    with pytest.raises(SystemExit) as error:
        QUALIFICATION.save_main()
    assert error.value.code == 2
    assert "inside the current workspace" in capsys.readouterr().err


def test_direct_verifier_rejects_an_outside_workspace_before_reading(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    outside = report(tmp_path)
    workspace = tmp_path / "nested workspace"
    workspace.mkdir()
    monkeypatch.chdir(workspace)
    with pytest.raises(ValueError, match="inside the current workspace"):
        QUALIFICATION.verify_report(outside)
