from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("sonar_ide_analysis", REPOSITORY_ROOT / "scripts" / "sonar_ide_analysis.py")
assert SPEC is not None
assert SPEC.loader is not None
sonar_ide_analysis = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = sonar_ide_analysis
SPEC.loader.exec_module(sonar_ide_analysis)


def test_select_files_keeps_only_existing_cpp_files_in_the_worktree(tmp_path: Path) -> None:
    source = tmp_path / "src" / "renderer.cpp"
    source.parent.mkdir()
    source.write_text("int main() {}", encoding="utf-8")
    note = tmp_path / "notes.txt"
    note.write_text("not source", encoding="utf-8")

    outside = tmp_path.parent / "outside.cpp"
    selection = sonar_ide_analysis.select_files(tmp_path, [source, note, Path("missing.cpp"), outside], "explicit")

    assert selection.submitted == [source]
    assert selection.skipped == [
        {"path": str(note), "reason": "unsupported_file_type"},
        {"path": "missing.cpp", "reason": "missing_or_not_regular_file"},
        {"path": str(outside), "reason": "outside_worktree"},
    ]


def test_validate_compile_commands_requires_sources_but_allows_headers(tmp_path: Path) -> None:
    compiled = tmp_path / "compiled.cpp"
    missing = tmp_path / "missing.cpp"
    header = tmp_path / "public.hpp"
    database = tmp_path / "compile_commands.json"
    database.write_text(json.dumps([{"directory": str(tmp_path), "file": str(compiled), "command": "c++ -c compiled.cpp"}]), encoding="utf-8")

    sonar_ide_analysis.validate_compile_commands(database, [compiled, header])
    with pytest.raises(sonar_ide_analysis.AnalysisError, match="no command for submitted"):
        sonar_ide_analysis.validate_compile_commands(database, [missing])


def test_write_workspace_keeps_generated_state_inside_the_build_directory(tmp_path: Path) -> None:
    build = Path("build/sonar-local")
    (tmp_path / build).mkdir(parents=True)

    workspace = sonar_ide_analysis.write_workspace(tmp_path, build, "horocore", "horocore_horo-engine")
    payload = json.loads(workspace.read_text(encoding="utf-8"))

    assert workspace == tmp_path / build / "sonar-ide.code-workspace"
    assert payload["folders"] == [{"path": str(tmp_path)}]
    assert payload["settings"]["sonarlint.connectedMode.project"]["projectKey"] == "horocore_horo-engine"


def test_open_workspace_selects_only_the_new_bridge(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    snapshots = iter([{64120}, {64120}, {64120, 64121}])
    monkeypatch.setattr(sonar_ide_analysis, "available_bridge_ports", lambda: next(snapshots))
    monkeypatch.setattr(sonar_ide_analysis, "run_command", lambda *_: None)
    monkeypatch.setattr(sonar_ide_analysis.time, "sleep", lambda *_: None)

    workspace = tmp_path / "sonar.code-workspace"
    assert sonar_ide_analysis.open_workspace_and_find_bridge(workspace, 5) == 64121
    marker = workspace.parent / f"{workspace.name}.bridge.json"
    assert json.loads(marker.read_text(encoding="utf-8")) == {"port": 64121}


def test_open_workspace_reuses_its_remembered_live_bridge(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    workspace = tmp_path / "sonar.code-workspace"
    marker = workspace.parent / f"{workspace.name}.bridge.json"
    marker.write_text('{"port": 64122}\n', encoding="utf-8")
    monkeypatch.setattr(sonar_ide_analysis, "available_bridge_ports", lambda: {64120, 64122})
    monkeypatch.setattr(sonar_ide_analysis, "run_command", lambda *_: None)

    assert sonar_ide_analysis.open_workspace_and_find_bridge(workspace, 5) == 64122


def test_bridge_url_rejects_ports_outside_the_ide_range() -> None:
    with pytest.raises(sonar_ide_analysis.AnalysisError, match="64120-64130"):
        sonar_ide_analysis.bridge_url(64119, "/sonarlint/api/status")


def test_analyze_when_indexed_retries_only_the_startup_response(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    responses: list[object] = [
        sonar_ide_analysis.AnalysisError(
            "IDE bridge returned HTTP 500: Failed to analyze files, reason: No files were found to be indexed by SonarQube for IDE"
        ),
        {"findings": [{"ruleKey": "cpp:S1"}]},
    ]

    def request(*_: object) -> object:
        response = responses.pop(0)
        if isinstance(response, Exception):
            raise response
        return response

    monkeypatch.setattr(sonar_ide_analysis, "request_bridge", request)
    monkeypatch.setattr(sonar_ide_analysis.time, "sleep", lambda *_: None)

    assert sonar_ide_analysis.analyze_when_indexed(64120, [tmp_path / "source.cpp"], 30, 5) == [{"ruleKey": "cpp:S1"}]


def test_filter_findings_to_ranges_suppresses_unchanged_lines(tmp_path: Path) -> None:
    source = tmp_path / "source.cpp"
    findings = [
        {"filePath": str(source), "textRange": {"startLine": 4, "endLine": 4}},
        {"filePath": str(source), "textRange": {"startLine": 12, "endLine": 13}},
    ]

    selected, suppressed = sonar_ide_analysis.filter_findings_to_ranges(findings, {source: [(10, 14)]})

    assert selected == [findings[1]]
    assert suppressed == 1


def test_main_rejects_combining_explicit_files_with_a_change_selector(capsys: pytest.CaptureFixture[str]) -> None:
    exit_code = sonar_ide_analysis.main(["--port", "64120", "--base", "origin/main", "src/example.cpp"])

    assert exit_code == 2
    assert json.loads(capsys.readouterr().err) == {
        "status": "error",
        "error": "Explicit files cannot be combined with a change selector",
    }
