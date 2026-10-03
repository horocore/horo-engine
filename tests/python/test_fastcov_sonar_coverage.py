"""Contracts for the authoritative fastcov line coverage conversion."""
import importlib.util
import json
from pathlib import Path
import sys

import pytest

element_tree = pytest.importorskip("defusedxml.ElementTree")
SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPTS))
try:
    SPEC = importlib.util.spec_from_file_location("fastcov_sonar_coverage", SCRIPTS / "fastcov_sonar_coverage.py")
    converter = importlib.util.module_from_spec(SPEC)
    SPEC.loader.exec_module(converter)
finally:
    sys.path.pop(0)


@pytest.mark.parametrize("text", ["", "{", "}", "else", "  } // comment", "/* comment */ { "])
def test_noncode_policy(text: str) -> None:
    assert converter.noncode(text)


@pytest.mark.parametrize("text", ["return {};", "} else {", "callback();", "};"])
def test_real_code_is_retained(text: str) -> None:
    assert not converter.noncode(text)


def test_merges_templates_retains_untested_code_and_filters_only_zero_hit_noncode(tmp_path: Path) -> None:
    source = tmp_path / "source.cpp"
    source.write_text("{\nreturn 7;\ncallback();\n}\n}")
    data = {"sources": {str(source): {
        "first": {"lines": {"1": 0, "2": 0, "3": 0, "4": 0, "5": 1}},
        "second": {"lines": {"2": 13}},
    }}}
    document, metrics = converter.convert(data, tmp_path)
    assert metrics == {"lines": 3, "covered": 2, "excludedNoncode": 2}
    lines = document.find("file").findall("lineToCover")
    assert [line.attrib for line in lines] == [
        {"lineNumber": "2", "covered": "true"},
        {"lineNumber": "3", "covered": "false"},
        {"lineNumber": "5", "covered": "true"},
    ]
    assert document.find("file").get("path") == "source.cpp"


@pytest.mark.parametrize("line,count", [("0", 1), ("1", -1), ("1", True), ("1", 1.5)])
def test_invalid_counters_are_rejected(line: str, count: object) -> None:
    with pytest.raises(ValueError):
        converter.source_counts({"": {"lines": {line: count}}})


def test_empty_reports_fail() -> None:
    with pytest.raises(ValueError):
        converter.convert({"sources": {}}, Path.cwd())


def test_outside_sources_and_symlinks_fail(tmp_path: Path) -> None:
    root = tmp_path / "repo"
    root.mkdir()
    outside = tmp_path / "outside.cpp"
    outside.write_text("callback();")
    link = root / "link.cpp"
    link.symlink_to(outside)
    for path in (outside, link):
        with pytest.raises(ValueError):
            converter.convert({"sources": {str(path): {"": {"lines": {"1": 1}}}}}, root)


def test_invalid_source_line_fails(tmp_path: Path) -> None:
    source = tmp_path / "source.cpp"
    source.write_text("callback();")
    with pytest.raises(ValueError):
        converter.convert({"sources": {str(source): {"": {"lines": {"2": 1}}}}}, tmp_path)


def test_cli_outputs_line_xml(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.chdir(tmp_path)
    (tmp_path / "source.cpp").write_text("callback();")
    report = tmp_path / "counts.json"
    report.write_text(json.dumps({"sources": {str(tmp_path / "source.cpp"): {"": {"lines": {"1": 1}}}}}))
    output = tmp_path / "coverage.xml"
    monkeypatch.setattr(sys, "argv", ["converter", "--input", str(report), "--output", str(output)])
    assert converter.main() == 0
    document = element_tree.parse(output).getroot()
    assert document.get("version") == "1"
    assert document.find("file/lineToCover").get("covered") == "true"


def test_cli_output_traversal_fails(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    root = tmp_path / "repo"
    root.mkdir()
    monkeypatch.chdir(root)
    outside = tmp_path / "outside.xml"
    monkeypatch.setattr(sys, "argv", ["converter", "--input", "missing.json", "--output", str(outside)])
    with pytest.raises(SystemExit) as error:
        converter.main()
    assert error.value.code == 2
    assert not outside.exists()
