from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path
from types import SimpleNamespace

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

    sonar_ide_analysis.validate_compile_commands(tmp_path, database, [compiled, header])
    with pytest.raises(sonar_ide_analysis.AnalysisError, match="no command for submitted"):
        sonar_ide_analysis.validate_compile_commands(tmp_path, database, [missing])


def test_validate_compile_commands_rejects_invalid_and_outside_entries(tmp_path: Path) -> None:
    source = tmp_path / "source.cpp"
    database = tmp_path / sonar_ide_analysis.COMPILE_COMMANDS_FILENAME
    database.write_text(json.dumps(["invalid", {"file": str(tmp_path.parent / "outside.cpp")}]), encoding="utf-8")

    with pytest.raises(sonar_ide_analysis.AnalysisError, match="no command for submitted"):
        sonar_ide_analysis.validate_compile_commands(tmp_path, database, [source])

    database.write_text("{}", encoding="utf-8")
    with pytest.raises(sonar_ide_analysis.AnalysisError, match="not an array"):
        sonar_ide_analysis.validate_compile_commands(tmp_path, database, [])


def test_resolved_tool_and_run_command_enforce_fixed_successful_tools(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(sonar_ide_analysis.shutil, "which", lambda name: f"/tools/{name}")
    monkeypatch.setattr(
        sonar_ide_analysis.subprocess,
        "run",
        lambda command, **_: SimpleNamespace(returncode=0, stdout="ok", stderr="", args=command),
    )

    assert sonar_ide_analysis.resolved_tool("git") == "/tools/git"
    assert sonar_ide_analysis.run_command("cmake", ["--version"], "failed").stdout == "ok"
    with pytest.raises(sonar_ide_analysis.AnalysisError, match="Unsupported prerequisite"):
        sonar_ide_analysis.resolved_tool("shell")


def test_run_command_reports_missing_tools_and_failures(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(sonar_ide_analysis.shutil, "which", lambda _: None)
    with pytest.raises(sonar_ide_analysis.AnalysisError, match="code is required"):
        sonar_ide_analysis.resolved_tool("code")

    monkeypatch.setattr(sonar_ide_analysis, "resolved_tool", lambda _: "/tools/cmake")
    monkeypatch.setattr(
        sonar_ide_analysis.subprocess,
        "run",
        lambda *_args, **_kwargs: SimpleNamespace(returncode=1, stdout="", stderr="configuration failed"),
    )
    with pytest.raises(sonar_ide_analysis.AnalysisError, match="configuration failed"):
        sonar_ide_analysis.run_command("cmake", [], "CMake failed")


@pytest.mark.parametrize("value", ["--help", "topic..parent", "topic//child", "topic/", "topic."])
def test_validated_git_ref_rejects_unsafe_spellings(value: str) -> None:
    with pytest.raises(sonar_ide_analysis.AnalysisError, match="conventional Git ref"):
        sonar_ide_analysis.validated_git_ref(value)


def test_validated_git_ref_accepts_stack_branch() -> None:
    assert sonar_ide_analysis.validated_git_ref("feat/HORO-95_asset_preview_extension") == "feat/HORO-95_asset_preview_extension"


def test_changed_paths_selects_base_and_dirty_changes(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    def base_git(_root: Path, *arguments: str) -> bytes:
        if arguments[0] == "merge-base":
            return b"0123456789abcdef0123456789abcdef01234567\n"
        if arguments[0] == "diff":
            return b"src/base.cpp\0"
        return b"src/untracked.cpp\0"

    monkeypatch.setattr(sonar_ide_analysis, "run_git", base_git)
    paths, source = sonar_ide_analysis.changed_paths(tmp_path, "origin/main")
    assert paths == [Path("src/base.cpp"), Path("src/untracked.cpp")]
    assert source == "base:origin/main"

    monkeypatch.setattr(sonar_ide_analysis, "run_git", lambda *_args: b" M src/dirty.cpp\0?? src/new.hpp\0")
    paths, source = sonar_ide_analysis.changed_paths(tmp_path, None)
    assert paths == [Path("src/dirty.cpp"), Path("src/new.hpp")]
    assert source == "dirty"


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
    with pytest.raises(sonar_ide_analysis.AnalysisError, match="Unsupported"):
        sonar_ide_analysis.bridge_url(64120, "/untrusted")


def test_normalized_findings_validates_the_bridge_shape() -> None:
    assert sonar_ide_analysis.normalized_findings({"findings": [{"ruleKey": "cpp:S1"}]}) == [{"ruleKey": "cpp:S1"}]
    with pytest.raises(sonar_ide_analysis.AnalysisError, match="findings array"):
        sonar_ide_analysis.normalized_findings({})
    with pytest.raises(sonar_ide_analysis.AnalysisError, match="invalid finding"):
        sonar_ide_analysis.normalized_findings({"findings": ["invalid"]})


def test_request_bridge_uses_only_the_validated_loopback_endpoint(monkeypatch: pytest.MonkeyPatch) -> None:
    requests: list[object] = []

    class Response:
        status = 200

        def __enter__(self) -> "Response":
            return self

        def __exit__(self, *_args: object) -> None:
            return None

        @staticmethod
        def read() -> bytes:
            return b'{"findings": []}'

    def open_request(request: object, timeout: float) -> Response:
        requests.append((request, timeout))
        return Response()

    monkeypatch.setattr(sonar_ide_analysis, "urlopen", open_request)

    assert sonar_ide_analysis.request_bridge(64120, sonar_ide_analysis.BRIDGE_STATUS_PATH, 1) == 200
    assert sonar_ide_analysis.request_bridge(64120, sonar_ide_analysis.BRIDGE_ANALYZE_PATH, 2, b"{}") == {"findings": []}
    assert len(requests) == 2


def test_available_bridge_ports_ignores_unreachable_ports(monkeypatch: pytest.MonkeyPatch) -> None:
    def request(port: int, *_args: object) -> int:
        if port != 64123:
            raise sonar_ide_analysis.AnalysisError("offline")
        return 200

    monkeypatch.setattr(sonar_ide_analysis, "request_bridge", request)
    assert sonar_ide_analysis.available_bridge_ports() == {64123}


def test_parse_changed_line_ranges_ignores_deletions_and_tracks_additions(tmp_path: Path) -> None:
    diff = "\n".join(
        [
            "+++ b/src/example.cpp",
            "@@ -4,2 +4,0 @@",
            "@@ -10,0 +9,3 @@",
            "+++ /dev/null",
            "@@ -1 +0,0 @@",
        ]
    )

    assert sonar_ide_analysis.parse_changed_line_ranges(tmp_path, diff) == {(tmp_path / "src/example.cpp").resolve(): [(9, 11)]}


def test_add_untracked_line_ranges_marks_complete_new_files(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    tracked = tmp_path / "tracked.cpp"
    untracked = tmp_path / "new.cpp"
    tracked.write_text("one\n", encoding="utf-8")
    untracked.write_text("one\ntwo\n", encoding="utf-8")
    monkeypatch.setattr(sonar_ide_analysis, "run_git", lambda *_args: b"tracked.cpp\0")
    ranges: dict[Path, list[tuple[int, int]]] = {}

    sonar_ide_analysis.add_untracked_line_ranges(tmp_path, [tracked, untracked], ranges)

    assert ranges == {untracked: [(1, 2)]}


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


def test_result_payload_reports_clean_selection(tmp_path: Path) -> None:
    source = tmp_path / "source.cpp"
    selection = sonar_ide_analysis.Selection([source], [{"path": "notes.md", "reason": "unsupported_file_type"}], "dirty")

    payload = sonar_ide_analysis.result_payload(selection, 64120, tmp_path / "commands.json", [], 3)

    assert payload["status"] == "clean"
    assert payload["baselineFindingsSuppressed"] == 3
    assert payload["selection"]["submitted"] == [str(source)]


def test_run_git_translates_process_failures(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(sonar_ide_analysis, "resolved_tool", lambda _: "/tools/git")

    def fail(*_args: object, **_kwargs: object) -> object:
        raise sonar_ide_analysis.subprocess.CalledProcessError(1, "git", stderr=b"bad ref")

    monkeypatch.setattr(sonar_ide_analysis.subprocess, "run", fail)
    with pytest.raises(sonar_ide_analysis.AnalysisError, match="bad ref"):
        sonar_ide_analysis.run_git(tmp_path, "status")


def test_main_rejects_combining_explicit_files_with_a_change_selector(capsys: pytest.CaptureFixture[str]) -> None:
    exit_code = sonar_ide_analysis.main(["--port", "64120", "--base", "origin/main", "src/example.cpp"])

    assert exit_code == 2
    assert json.loads(capsys.readouterr().err) == {
        "status": "error",
        "error": "Explicit files cannot be combined with a change selector",
    }


def test_main_reports_a_clean_explicit_analysis(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    source = tmp_path / "source.cpp"
    source.write_text("int value;\n", encoding="utf-8")
    build = tmp_path / "build"
    build.mkdir()
    (build / sonar_ide_analysis.COMPILE_COMMANDS_FILENAME).write_text(
        json.dumps([{"directory": str(tmp_path), "file": str(source), "command": "c++ -c source.cpp"}]), encoding="utf-8"
    )
    monkeypatch.setattr(sonar_ide_analysis, "repository_root", lambda: tmp_path)
    monkeypatch.setattr(sonar_ide_analysis, "request_bridge", lambda *_args: 200)
    monkeypatch.setattr(sonar_ide_analysis, "analyze_when_indexed", lambda *_args: [])

    exit_code = sonar_ide_analysis.main(
        ["--port", "64120", "--no-prepare", "--build-directory", str(build), str(source)]
    )

    assert exit_code == 0
    assert json.loads(capsys.readouterr().out)["status"] == "clean"
