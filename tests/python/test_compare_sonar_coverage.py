"""Regression coverage for the experimental coverage collector comparison."""

import importlib.util
import json
from pathlib import Path
import sys

import pytest


pytest.importorskip("defusedxml", reason="Sonar collector experiment dependency")

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "compare_sonar_coverage.py"
SPEC = importlib.util.spec_from_file_location("compare_sonar_coverage", SCRIPT)
comparison = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(comparison)


def report(path: Path, source: str, lines: str) -> Path:
    path.write_text(
        f'<coverage version="1"><file path="{source}">{lines}</file></coverage>',
        encoding="utf-8",
    )
    return path


def test_normalizes_absolute_and_relative_paths(tmp_path: Path) -> None:
    lines = '<lineToCover lineNumber="3" covered="false"/>'
    baseline = report(tmp_path / "baseline.xml", str(tmp_path / "src/example.cpp"), lines)
    candidate = report(tmp_path / "candidate.xml", "src/example.cpp", lines)
    result = comparison.compare_reports(
        comparison.read_report(baseline, tmp_path),
        comparison.read_report(candidate, tmp_path),
    )
    assert result["equivalent"]
    assert result["gcovr"] == {"lines": 1, "covered": 0}


def test_equal_percentages_do_not_hide_changed_line_coverage() -> None:
    baseline = {("src/example.cpp", 1): True, ("src/example.cpp", 2): False}
    candidate = {("src/example.cpp", 1): False, ("src/example.cpp", 2): True}
    result = comparison.compare_reports(baseline, candidate)
    assert not result["equivalent"]
    assert len(result["changedLines"]) == 2


def test_retains_missing_and_extra_uncovered_lines() -> None:
    result = comparison.compare_reports(
        {("include/example.h", 1): False}, {("src/example.cpp", 2): False}
    )
    assert not result["equivalent"]
    assert result["missingLines"] == [{"file": "include/example.h", "line": 1}]
    assert result["extraLines"] == [{"file": "src/example.cpp", "line": 2}]


@pytest.mark.parametrize("lines", [
    "",
    '<lineToCover lineNumber="0" covered="true"/>',
    '<lineToCover lineNumber="1" covered="unknown"/>',
])
def test_rejects_invalid_or_empty_reports(tmp_path: Path, lines: str) -> None:
    path = report(tmp_path / "coverage.xml", "src/example.cpp", lines)
    with pytest.raises(ValueError):
        comparison.read_report(path, tmp_path)


def test_rejects_outside_repository_paths(tmp_path: Path) -> None:
    path = report(tmp_path / "coverage.xml", "../outside.cpp",
                  '<lineToCover lineNumber="1" covered="true"/>')
    with pytest.raises(ValueError):
        comparison.read_report(path, tmp_path)


def test_rejects_entity_expansion(tmp_path: Path) -> None:
    path = tmp_path / "coverage.xml"
    path.write_text(
        '<!DOCTYPE coverage [<!ENTITY text "expanded">]>'
        '<coverage version="1"><file path="&text;"/></coverage>',
        encoding="utf-8",
    )
    with pytest.raises(comparison.DefusedXmlException):
        comparison.read_report(path, tmp_path)


@pytest.mark.parametrize("candidate_covered", ["true", "false"])
def test_main_writes_timing_and_complete_comparison(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture, candidate_covered: str
) -> None:
    baseline = report(tmp_path / "baseline.xml", "src/example.cpp",
                      '<lineToCover lineNumber="1" covered="true"/>')
    candidate = report(tmp_path / "candidate.xml", "src/example.cpp",
                       f'<lineToCover lineNumber="1" covered="{candidate_covered}"/>')
    output = tmp_path / "comparison.json"
    monkeypatch.chdir(tmp_path)
    monkeypatch.setattr(sys, "argv", [
        "compare_sonar_coverage.py", "--root", str(tmp_path),
        "--gcovr-report", str(baseline), "--fastcov-report", str(candidate),
        "--gcovr-seconds", "193", "--fastcov-seconds", "12",
        "--output", str(output),
    ])
    assert comparison.main() == 0
    result = json.loads(output.read_text(encoding="utf-8"))
    assert result["equivalent"] == (candidate_covered == "true")
    assert result["seconds"] == {"gcovr": 193, "fastcov": 12}
    assert "Sonar continues to use the gcovr baseline" in capsys.readouterr().out


def test_main_rejects_incomplete_input(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.chdir(tmp_path)
    missing = tmp_path / "missing.xml"
    output = tmp_path / "comparison.json"
    monkeypatch.setattr(sys, "argv", [
        "compare_sonar_coverage.py", "--root", str(tmp_path),
        "--gcovr-report", str(missing), "--fastcov-report", str(missing),
        "--gcovr-seconds", "193", "--fastcov-seconds", "12",
        "--output", str(output),
    ])
    with pytest.raises(SystemExit) as error:
        comparison.main()
    assert error.value.code == 2
    assert not output.exists()


@pytest.mark.parametrize("flags", [("false", "true"), ("true", "false"), ("false", "false")])
def test_combines_duplicate_instantiation_lines(tmp_path: Path, flags: tuple[str, str]) -> None:
    path = report(tmp_path / "coverage.xml", "src/example.cpp", "".join(
        f'<lineToCover lineNumber="1" covered="{flag}"/>' for flag in flags
    ))
    assert comparison.read_report(path, tmp_path) == {("src/example.cpp", 1): "true" in flags}


def test_rejects_input_output_and_symlink_escape(tmp_path: Path) -> None:
    root = tmp_path / "repo"
    root.mkdir()
    outside = tmp_path / "outside"
    outside.mkdir()
    (root / "link").symlink_to(outside, target_is_directory=True)
    for path in (outside / "report.xml", root / "link/report.xml"):
        with pytest.raises(ValueError):
            comparison.repository_path(path, root)


def test_main_rejects_outside_output(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    root = tmp_path / "repo"
    root.mkdir()
    monkeypatch.chdir(root)
    source = report(root / "coverage.xml", "src/example.cpp",
                    '<lineToCover lineNumber="1" covered="true"/>')
    outside = tmp_path / "outside.json"
    monkeypatch.setattr(sys, "argv", [
        "compare_sonar_coverage.py", "--root", str(root),
        "--gcovr-report", str(source), "--fastcov-report", str(source),
        "--gcovr-seconds", "1", "--fastcov-seconds", "1",
        "--output", str(outside),
    ])
    with pytest.raises(SystemExit) as error:
        comparison.main()
    assert error.value.code == 2
    assert not outside.exists()
