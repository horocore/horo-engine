"""Positive qualification requires both recovered Save generator branches."""
from __future__ import annotations

import importlib.util
from pathlib import Path
from html import escape

import defusedxml.ElementTree as etree
from defusedxml.common import DefusedXmlException

import pytest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "save_qualification", ROOT / "scripts/verify_save_allocation_qualification.py")
assert SPEC and SPEC.loader
QUALIFICATION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(QUALIFICATION)


def report(tmp_path: Path, *, omitted: str = "", outcome: str = "") -> Path:
    cases = []
    for name, phase in QUALIFICATION.CASES.items():
        output = []
        for variant in ("success", "failure"):
            markers = (f"{phase}-sync-{variant}", f"{phase}-failure-reset",
                       f"{phase}-reopen-returned", f"{phase}-reconcile-returned",
                       f"{phase}-fixture-cleanup-next", "fixture-scope-exited")
            output.extend(f"[save-allocation] {marker}\n" for marker in markers if marker != omitted)
        result = f"<{outcome}/>" if outcome else ""
        cases.append(f'<testcase name="{escape(QUALIFICATION.PREFIX + name, quote=True)}" status="run">'
                     f"{result}<system-out>{escape(''.join(output))}</system-out></testcase>")
    path = tmp_path / "qualification.xml"
    path.write_text('<testsuite tests="2" failures="0" skipped="0">' + "".join(cases) + "</testsuite>", encoding="utf-8")
    return path


def test_both_variants_and_completed_cleanup_qualify(tmp_path: Path) -> None:
    QUALIFICATION.verify_report(report(tmp_path))


@pytest.mark.parametrize("outcome", ["skipped", "failure", "error"])
def test_nonpassing_case_cannot_qualify(tmp_path: Path, outcome: str) -> None:
    with pytest.raises(ValueError, match="did not pass"):
        QUALIFICATION.verify_report(report(tmp_path, outcome=outcome))


@pytest.mark.parametrize("omitted", ["lifecycle-sync-failure", "retention-sync-success",
                                     "lifecycle-reopen-returned", "retention-reconcile-returned",
                                     "fixture-scope-exited"])
def test_partial_or_unfinished_variant_cannot_qualify(tmp_path: Path, omitted: str) -> None:
    with pytest.raises(ValueError, match="Incomplete"):
        QUALIFICATION.verify_report(report(tmp_path, omitted=omitted))


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
