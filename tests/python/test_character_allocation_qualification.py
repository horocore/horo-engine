"""The Windows Release Character route requires actual exact, non-skipped CTest cases."""

import importlib.util
from pathlib import Path
from html import escape

import pytest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "character_allocation_qualification", ROOT / "scripts/ci/run_character_allocation_qualification.py"
)
QUALIFICATION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(QUALIFICATION)


def discovery(names=None):
    return {"tests": [{"name": name, "properties": []} for name in (
        sorted(QUALIFICATION.CASES) if names is None else names
    )]}


def junit(tmp_path, names=None):
    cases = sorted(QUALIFICATION.CASES) if names is None else names
    body = "".join(f'<testcase name="{escape(name, quote=True)}" status="run" time="0.001"/>' for name in cases)
    path = tmp_path / "results.xml"
    path.write_text(f'<testsuite failures="0" errors="0" skipped="0">{body}</testsuite>', encoding="utf-8")
    return path


def test_exact_preparation_release_case_is_accepted(tmp_path):
    assert len(QUALIFICATION.CASES) == 1
    QUALIFICATION.verify_discovery(discovery())
    QUALIFICATION.verify_junit(junit(tmp_path))


@pytest.mark.parametrize("names", [[], sorted(QUALIFICATION.CASES)[1:],
                                   sorted(QUALIFICATION.CASES) + [sorted(QUALIFICATION.CASES)[0]]])
def test_missing_zero_or_duplicate_discovery_is_rejected(names):
    with pytest.raises(ValueError):
        QUALIFICATION.verify_discovery(discovery(names))


def test_disabled_release_case_is_rejected():
    document = discovery()
    document["tests"][0]["properties"] = [{"name": "DISABLED", "value": True}]
    with pytest.raises(ValueError, match="disabled"):
        QUALIFICATION.verify_discovery(document)


@pytest.mark.parametrize("names", [[], sorted(QUALIFICATION.CASES)[1:],
                                   sorted(QUALIFICATION.CASES) + ["unexpected case"]])
def test_missing_zero_or_extra_junit_cases_are_rejected(tmp_path, names):
    with pytest.raises(ValueError):
        QUALIFICATION.verify_junit(junit(tmp_path, names))


@pytest.mark.parametrize("tag", ["failure", "error", "skipped"])
def test_failed_or_skipped_case_cannot_qualify(tmp_path, tag):
    path = junit(tmp_path)
    text = path.read_text(encoding="utf-8")
    path.write_text(text.replace("/>", f"><{tag}/></testcase>", 1), encoding="utf-8")
    with pytest.raises(ValueError, match="did not pass"):
        QUALIFICATION.verify_junit(path)


def test_renamed_template_instance_cannot_replace_expected_case(tmp_path):
    names = sorted(QUALIFICATION.CASES)
    names[0] += " renamed"
    with pytest.raises(ValueError):
        QUALIFICATION.verify_junit(junit(tmp_path, names))


def test_release_step_is_independent_of_debug_test_outcome():
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    section = workflow.split("- name: Test Windows Release Character allocation qualification", 1)[1]
    block = section.split("\n      - name:", 1)[0]
    assert "!cancelled()" in block
    assert "steps.mcp_release_build.outcome == 'success'" in block
    assert "debug" not in block.lower()
    assert "run_character_allocation_qualification.py" in block


def test_unexpected_template_instantiation_is_not_silently_omitted():
    names = sorted(QUALIFICATION.CASES)
    names.append(QUALIFICATION.PREFIX + "Character preparation allocation failures roll back every retained storage stage - ExtraVariant")
    with pytest.raises(ValueError):
        QUALIFICATION.verify_discovery(discovery(names))
