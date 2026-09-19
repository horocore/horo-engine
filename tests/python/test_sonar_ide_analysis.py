from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path
from urllib.error import URLError

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

    selection = sonar_ide_analysis.select_files(tmp_path, [source, note, Path("missing.cpp"), Path("/tmp/outside.cpp")], "explicit")

    assert selection.submitted == [source]
    assert selection.skipped == [
        {"path": str(note), "reason": "unsupported_file_type"},
        {"path": "missing.cpp", "reason": "missing_or_not_regular_file"},
        {"path": "/tmp/outside.cpp", "reason": "outside_worktree"},
    ]


def test_bridge_url_rejects_ports_outside_the_ide_range() -> None:
    with pytest.raises(sonar_ide_analysis.AnalysisError, match="64120-64130"):
        sonar_ide_analysis.bridge_url(64119, "/sonarlint/api/status")


def test_validate_compile_commands_rejects_a_file_without_a_compile_command(tmp_path: Path) -> None:
    compiled = tmp_path / "compiled.cpp"
    missing = tmp_path / "missing.cpp"
    database = tmp_path / "compile_commands.json"
    database.write_text(json.dumps([{"directory": str(tmp_path), "file": str(compiled), "command": "c++ -c compiled.cpp"}]), encoding="utf-8")

    with pytest.raises(sonar_ide_analysis.AnalysisError, match="no command for submitted"):
        sonar_ide_analysis.validate_compile_commands(database, [missing])


def test_normalized_findings_rejects_malformed_bridge_responses() -> None:
    with pytest.raises(sonar_ide_analysis.AnalysisError, match="findings array"):
        sonar_ide_analysis.normalized_findings({"issues": []})


def test_request_bridge_uses_the_official_localhost_headers(monkeypatch: pytest.MonkeyPatch) -> None:
    captured: dict[str, object] = {}

    class Response:
        status = 200

        def __enter__(self) -> Response:
            return self

        def __exit__(self, *_: object) -> None:
            return None

        def read(self) -> bytes:
            return b'{"findings": []}'

    def fake_urlopen(request: object, timeout: float) -> Response:
        captured["request"] = request
        captured["timeout"] = timeout
        return Response()

    monkeypatch.setattr(sonar_ide_analysis, "urlopen", fake_urlopen)

    assert sonar_ide_analysis.request_bridge("http://127.0.0.1:64120/sonarlint/api/analysis/files", 30, b"{}") == {"findings": []}
    request = captured["request"]
    assert request.get_header("Host") == "localhost"
    assert request.get_header("Origin") == "http://localhost"
    assert request.get_header("Content-type") == "application/json"
    assert captured["timeout"] == 30


def test_request_bridge_reports_connection_failures(monkeypatch: pytest.MonkeyPatch) -> None:
    def failing_urlopen(*_: object, **__: object) -> object:
        raise URLError("connection refused")

    monkeypatch.setattr(sonar_ide_analysis, "urlopen", failing_urlopen)

    with pytest.raises(sonar_ide_analysis.AnalysisError, match="Cannot reach"):
        sonar_ide_analysis.request_bridge("http://127.0.0.1:64120/sonarlint/api/status", 30)


def test_result_payload_reports_issues_and_skipped_files(tmp_path: Path) -> None:
    source = tmp_path / "source.cpp"
    selection = sonar_ide_analysis.Selection([source], [{"path": "note.txt", "reason": "unsupported_file_type"}], "explicit")

    payload = sonar_ide_analysis.result_payload(selection, 64120, tmp_path / "compile_commands.json", [{"ruleKey": "cpp:S1"}])

    assert json.loads(json.dumps(payload))["status"] == "issues_found"
    assert payload["selection"]["skipped"] == [{"path": "note.txt", "reason": "unsupported_file_type"}]


def test_main_rejects_combining_explicit_files_with_a_change_selector(capsys: pytest.CaptureFixture[str]) -> None:
    exit_code = sonar_ide_analysis.main(["--port", "64120", "--base", "origin/main", "src/example.cpp"])

    assert exit_code == 2
    assert json.loads(capsys.readouterr().err) == {
        "status": "error",
        "error": "Explicit files cannot be combined with --base or --dirty",
    }
