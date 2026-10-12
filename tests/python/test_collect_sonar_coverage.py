"""Bounded collection preserves every input, counter and publication boundary."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

import pytest

pytest.importorskip("defusedxml.ElementTree")
SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPTS))
try:
    SPEC = importlib.util.spec_from_file_location("collect_sonar_coverage", SCRIPTS / "collect_sonar_coverage.py")
    collector = importlib.util.module_from_spec(SPEC)
    SPEC.loader.exec_module(collector)
finally:
    sys.path.pop(0)


def fixture(tmp_path: Path, count: int = 130) -> tuple:
    root = tmp_path / "repo space örnek"
    build = root / "build" / "sonar"
    build.mkdir(parents=True)
    source = root / "src" / "shared template.cpp"
    source.parent.mkdir()
    source.write_text("callback();\nuntested();\n", encoding="utf-8")
    files = []
    for number in range(count):
        path = build / f"object {number:04d} örnek.gcno"
        path.touch()
        files.append(path)
    return root, build, source, files


def fake_fastcov(monkeypatch: pytest.MonkeyPatch, source: Path, calls: list, failure: str = "") -> None:
    def run(command: list, **options: object) -> None:
        assert options == {"cwd": source.parent.parent, "check": True}
        assert command[command.index("--jobs") + 1] == "2"
        calls.append(command)
        output = Path(command[command.index("--output") + 1])
        if "--gcda-files" in command:
            if failure == "batch" and len(calls) == 2:
                raise subprocess.CalledProcessError(143, command)
            inputs = command[command.index("--gcda-files") + 1:command.index("--include")]
            assert len(inputs) <= 64
            if failure == "missing":
                return
            hits = len(inputs)
        else:
            assert "--add-tracefile" in command
            if failure == "merge":
                raise subprocess.CalledProcessError(1, command)
            reports = command[command.index("--add-tracefile") + 1:command.index("--output")]
            hits = sum(json.loads(Path(report).read_text())["sources"][str(source)][""]["lines"]["1"] for report in reports)
        data = {"sources": {str(source): {"": {"lines": {"1": hits, "2": 0},
                                               "functions": {"shared": {"start_line": 1, "execution_count": hits}},
                                               "branches": {"1": [hits, 0]}}}}}
        if failure == "conversion" and "--add-tracefile" in command:
            data["sources"][str(source)][""]["lines"]["1"] = -1
        output.write_text(json.dumps(data), encoding="utf-8")
    monkeypatch.setattr(collector.subprocess, "run", run)


@pytest.mark.parametrize("count", [1, 64, 65, 130])
def test_all_inputs_once_bounded_workers_paths_filters_and_counters(tmp_path: Path, monkeypatch: pytest.MonkeyPatch, count: int) -> None:
    root, build, source, files = fixture(tmp_path, count)
    calls = []
    fake_fastcov(monkeypatch, source, calls)
    includes = [str(root / "src"), str(root / "apps" / "exact host.cpp")]
    metrics = collector.collect(root, build, "pinned fastcov", includes)
    observed = []
    for command in calls[:-1]:
        assert command[0] == "pinned fastcov"
        assert "--process-gcno" in command
        assert command[command.index("--include") + 1:command.index("--output")] == includes
        observed.extend(command[command.index("--gcda-files") + 1:command.index("--include")])
    assert observed == list(map(str, sorted(files)))
    assert len(set(observed)) == count
    assert metrics == {"inputs": count, "batches": (count + 63) // 64, "lines": 2, "covered": 1, "excludedNoncode": 0}
    data = json.loads((build / "fastcov-coverage.json").read_text())
    counters = data["sources"][str(source)][""]
    assert counters["lines"] == {"1": count, "2": 0}
    assert counters["functions"]["shared"]["execution_count"] == count
    assert counters["branches"]["1"] == [count, 0]
    document = collector.converter.element_tree.parse(build / "coverage.xml")
    assert [line.attrib for line in document.findall("file/lineToCover")] == [
        {"lineNumber": "1", "covered": "true"}, {"lineNumber": "2", "covered": "false"}]
    assert not list(build.glob("coverage-batches-*"))


@pytest.mark.parametrize("failure", ["batch", "merge", "conversion", "missing"])
@pytest.mark.parametrize("existing", [False, True])
def test_failure_never_publishes_partial_or_replaces_previous_reports(tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
                                                                    failure: str, existing: bool) -> None:
    root, build, source, _ = fixture(tmp_path)
    calls = []
    fake_fastcov(monkeypatch, source, calls, failure)
    outputs = [build / "fastcov-coverage.json", build / "coverage.xml"]
    if existing:
        for output in outputs:
            output.write_bytes(b"previous complete report")
    with pytest.raises((ValueError, subprocess.CalledProcessError)):
        collector.collect(root, build, "fastcov", [str(root / "src")])
    for output in outputs:
        if existing:
            assert output.read_bytes() == b"previous complete report"
        else:
            assert not output.exists()
    assert not list(build.glob("coverage-batches-*"))


def test_empty_inputs_fail_without_invoking_tools(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    root, build, source, _ = fixture(tmp_path, 0)
    calls = []
    fake_fastcov(monkeypatch, source, calls)
    with pytest.raises(ValueError, match="No gcno"):
        collector.collect(root, build, "fastcov", [str(root / "src")])
    assert not calls


def test_build_outside_repository_is_rejected(tmp_path: Path) -> None:
    root, _, _, _ = fixture(tmp_path, 0)
    with pytest.raises(ValueError, match="inside the repository"):
        collector.collect(root, tmp_path, "fastcov", [str(root / "src")])


def test_publication_io_failure_is_terminal_not_a_transactional_pair(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    root, build, source, _ = fixture(tmp_path, 1)
    fake_fastcov(monkeypatch, source, [])
    json_output = build / "fastcov-coverage.json"
    xml_output = build / "coverage.xml"
    json_output.write_bytes(b"previous JSON")
    xml_output.write_bytes(b"previous XML")
    replace = collector.os.replace

    def fail_xml(candidate: Path, output: Path) -> None:
        if output == xml_output:
            raise OSError("publication I/O failure")
        replace(candidate, output)

    monkeypatch.setattr(collector.os, "replace", fail_xml)
    monkeypatch.setattr(sys, "argv", ["collector", "--root", str(root), "--build-directory", str(build),
                                      "--fastcov", "fastcov", "--include", str(root / "src")])
    with pytest.raises(SystemExit) as error:
        collector.main()
    assert error.value.code == 2
    assert json.loads(json_output.read_text())["sources"]
    assert xml_output.read_bytes() == b"previous XML"
    assert not list(build.glob("coverage-batches-*"))
    workflow = (SCRIPTS.parent / ".github/workflows/sonar.yml").read_text()
    assert "!cancelled() && steps.coverage.outcome == 'success'" in workflow
