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
    document = discovery(names)
    with pytest.raises(ValueError):
        QUALIFICATION.verify_discovery(document)


def test_disabled_release_case_is_rejected():
    document = discovery()
    document["tests"][0]["properties"] = [{"name": "DISABLED", "value": True}]
    with pytest.raises(ValueError, match="disabled"):
        QUALIFICATION.verify_discovery(document)


@pytest.mark.parametrize("names", [[], sorted(QUALIFICATION.CASES)[1:],
                                   sorted(QUALIFICATION.CASES) + ["unexpected case"]])
def test_missing_zero_or_extra_junit_cases_are_rejected(tmp_path, names):
    path = junit(tmp_path, names)
    with pytest.raises(ValueError):
        QUALIFICATION.verify_junit(path)


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
    path = junit(tmp_path, names)
    with pytest.raises(ValueError):
        QUALIFICATION.verify_junit(path)


def test_release_step_is_independent_of_debug_test_outcome():
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    section = workflow.split("- name: Test Windows Release Character allocation qualification", 1)[1]
    block = section.split("\n      - name:", 1)[0]
    assert "!cancelled()" in block
    assert "steps.mcp_release_build.outcome == 'success'" in block
    assert "debug" not in block.lower()
    assert "run_character_allocation_qualification.py" in block
    install = workflow.split("- name: Install native Character allocation verifier dependency", 1)[1]
    install = install.split("\n      - name:", 1)[0]
    assert "defusedxml==0.7.1" in install
    assert "--no-deps" in install
    assert workflow.index("Install native Character allocation verifier dependency") < workflow.index("- name: Test Debug")


def test_unexpected_template_instantiation_is_not_silently_omitted():
    names = sorted(QUALIFICATION.CASES)
    names.append(QUALIFICATION.PREFIX + "Character preparation allocation failures roll back every retained storage stage - ExtraVariant")
    document = discovery(names)
    with pytest.raises(ValueError):
        QUALIFICATION.verify_discovery(document)


def test_actual_ctest_path_contract_is_registered_and_matches_workflow():
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    step = workflow.split("- name: Test Windows Release Character allocation qualification", 1)[1]
    step = step.split("\n      - name:", 1)[0]
    assert '--output-junit "character-allocation-ctest.xml"' in step
    assert '--output-junit "$root/character-allocation-ctest.xml"' in step
    cmake = (ROOT / "tests/cmake/CharacterAllocationJunitPathContract.cmake").read_text(encoding="utf-8")
    assert '--test-dir "build/ci-audio-release"' in cmake
    assert '--output-junit "character-allocation-ctest.xml"' in cmake
    assert 'WORKING_DIRECTORY "${TEST_ROOT}"' in cmake
    registration = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    assert registration.index("add_test(NAME HoroCharacterAllocationJunitPathContract") < registration.index(
        "horo_finalize_ci_suites()"
    )
    assert "HoroCharacterAllocationJunitPathContract" in (ROOT / "tests/cmake/HoroCiSuites.cmake").read_text(encoding="utf-8")


def test_cli_paths_cannot_escape_workspace(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    outside = tmp_path.parent / "outside.xml"
    with pytest.raises(QUALIFICATION.argparse.ArgumentTypeError):
        QUALIFICATION.workspace_path(outside)
    assert QUALIFICATION.workspace_path("inside.xml") == tmp_path / "inside.xml"


def test_cli_paths_reject_symlink_escape(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    link = tmp_path / "outside"
    try:
        link.symlink_to(tmp_path.parent, target_is_directory=True)
    except OSError:
        pytest.skip("Host does not permit test symlink creation")
    escaped = link / "evidence.xml"
    with pytest.raises(QUALIFICATION.argparse.ArgumentTypeError):
        QUALIFICATION.workspace_path(escaped)


@pytest.mark.parametrize("absolute", [False, True])
def test_actual_cli_reads_workspace_junit_with_spaces(tmp_path, monkeypatch, absolute):
    monkeypatch.chdir(tmp_path)
    folder = tmp_path / "evidence with spaces"
    folder.mkdir()
    report = junit(folder)
    argument = str(report if absolute else report.relative_to(tmp_path))
    monkeypatch.setattr("sys.argv", ["qualification", "--output-junit", argument])
    QUALIFICATION.main()


@pytest.mark.parametrize("absolute", [False, True])
def test_actual_cli_rejects_outside_evidence(tmp_path, monkeypatch, absolute):
    monkeypatch.chdir(tmp_path)
    outside = tmp_path.parent / "outside.xml"
    argument = str(outside) if absolute else "../outside.xml"
    monkeypatch.setattr("sys.argv", ["qualification", "--output-junit", argument])
    with pytest.raises(SystemExit) as rejected:
        QUALIFICATION.main()
    assert rejected.value.code == 2
