from __future__ import annotations


import io
import json
import sys
from email.message import Message
from pathlib import Path, PurePosixPath, PureWindowsPath
from types import SimpleNamespace

import pytest

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY_ROOT / "scripts"))
# Reuse the canonical module so consumers share its AnalysisError identity.
import analyze as sonar_ide_analysis


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


def test_validate_compile_commands_resolves_entries_relative_to_database(tmp_path: Path) -> None:
    source = tmp_path / "relative.cpp"
    database = tmp_path / sonar_ide_analysis.COMPILE_COMMANDS_FILENAME
    database.write_text(json.dumps([{"file": source.name, "command": "c++ -c relative.cpp"}]), encoding="utf-8")

    sonar_ide_analysis.validate_compile_commands(tmp_path, database, [source])


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
    monkeypatch.setattr(sonar_ide_analysis.shutil, 'which', lambda name: f"/tools/{name}")
    calls: list[tuple[object, dict[str, object]]] = []

    def run(command: object, **kwargs: object) -> SimpleNamespace:
        calls.append((command, kwargs))
        return SimpleNamespace(returncode=0, stdout="ok", stderr="")

    monkeypatch.setattr(
        sonar_ide_analysis.subprocess,
        'run',
        run,
    )

    assert sonar_ide_analysis.resolved_tool("git") == "/tools/git"
    assert sonar_ide_analysis.run_command("cmake", ["--version"], "failed").stdout == "ok"
    assert calls == [
        (
            ["cmake", "--version"],
            {
                "executable": "/tools/cmake",
                "check": False,
                "capture_output": True,
                "text": True,
                "shell": False,
            },
        )
    ]
    with pytest.raises(sonar_ide_analysis.AnalysisError, match="Unsupported prerequisite"):
        sonar_ide_analysis.resolved_tool("shell")


def test_run_command_reports_missing_tools_and_failures(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(sonar_ide_analysis.shutil, 'which', lambda _: None)
    with pytest.raises(sonar_ide_analysis.AnalysisError, match="code is required"):
        sonar_ide_analysis.resolved_tool("code")

    monkeypatch.setattr(sonar_ide_analysis, 'resolved_tool', lambda _: "/tools/cmake")
    monkeypatch.setattr(
        sonar_ide_analysis.subprocess,
        'run',
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

    monkeypatch.setattr(sonar_ide_analysis, 'run_git', base_git)
    paths, source = sonar_ide_analysis.changed_paths(tmp_path, "origin/main")
    assert paths == [Path("src/base.cpp"), Path("src/untracked.cpp")]
    assert source == "base:origin/main"

    monkeypatch.setattr(sonar_ide_analysis, 'run_git', lambda *_args: b" M src/dirty.cpp\0?? src/new.hpp\0")
    paths, source = sonar_ide_analysis.changed_paths(tmp_path, None)
    assert paths == [Path("src/dirty.cpp"), Path("src/new.hpp")]
    assert source == "dirty"


def test_write_workspace_keeps_generated_state_outside_the_source(tmp_path: Path) -> None:
    root = tmp_path / "source"
    root.mkdir()
    cache = tmp_path / "managed"
    cache.mkdir()
    database = root / "build" / "compile_commands.json"
    workspace = sonar_ide_analysis.write_workspace(root, cache, database, "horocore", "horocore_horo-engine")
    payload = json.loads(workspace.read_text(encoding="utf-8"))
    assert workspace == cache / "sonar.code-workspace"
    assert payload["folders"] == [{"path": str(root)}]
    assert payload["settings"]["sonarlint.pathToCompileCommands"] == str(database)
    assert payload["settings"]["sonarlint.connectedMode.project"]["projectKey"] == "horocore_horo-engine"
    assert list(root.iterdir()) == []


def test_obsolete_port_discovery_has_no_public_authority() -> None:
    assert not hasattr(sonar_ide_analysis, "open_workspace_and_find_bridge")
    assert not hasattr(sonar_ide_analysis, "_analysis_bridge")


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

    monkeypatch.setattr(sonar_ide_analysis, 'urlopen', open_request)

    assert sonar_ide_analysis.request_bridge(64120, sonar_ide_analysis.BRIDGE_STATUS_PATH, 1) == 200
    assert sonar_ide_analysis.request_bridge(64120, sonar_ide_analysis.BRIDGE_ANALYZE_PATH, 2, b"{}") == {"findings": []}
    assert len(requests) == 2


def test_no_unauthenticated_port_scan_is_exposed() -> None:
    assert not hasattr(sonar_ide_analysis, "available_bridge_ports")
    assert not hasattr(sonar_ide_analysis, "bridge_main")


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
    monkeypatch.setattr(sonar_ide_analysis, 'run_git', lambda *_args: b"tracked.cpp\0")
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

    monkeypatch.setattr(sonar_ide_analysis, 'request_bridge', request)
    monkeypatch.setattr(sonar_ide_analysis.CANCELLED, 'wait', lambda *_: False)

    assert sonar_ide_analysis.analyze_when_indexed(64120, [tmp_path / "source.cpp"], 30, 5) == [{"ruleKey": "cpp:S1"}]


def test_bridge_indexing_and_rate_limit_backoff_preserves_findings(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    clock = [0.0]
    waits: list[float] = []
    requests: list[object] = []
    headers = Message()
    headers['Retry-After'] = '7'
    responses = [
        sonar_ide_analysis.HTTPError('http://127.0.0.1:64120', 500, 'indexing', None,
                                    io.BytesIO(b'No files were found to be indexed by SonarQube for IDE')),
        sonar_ide_analysis.HTTPError('http://127.0.0.1:64120', 429, 'busy', headers, io.BytesIO(b'')),
    ]

    class Response:
        def __enter__(self) -> 'Response':
            return self

        def __exit__(self, *_: object) -> None:
            return None

        def read(self) -> bytes:
            return b'{"findings": [{"ruleKey": "cpp:S1"}]}'

    def request(value: object, **_: object) -> Response:
        requests.append(value)
        if responses:
            raise responses.pop(0)
        return Response()

    def wait(delay: float) -> bool:
        waits.append(delay)
        clock[0] += delay
        return False

    monkeypatch.setattr(sonar_ide_analysis, 'urlopen', request)
    monkeypatch.setattr(sonar_ide_analysis.time, 'monotonic', lambda: clock[0])
    monkeypatch.setattr(sonar_ide_analysis.CANCELLED, 'wait', wait)
    assert sonar_ide_analysis.analyze_when_indexed(64120, [tmp_path / 'source.cpp'], 30, 20) == [{'ruleKey': 'cpp:S1'}]
    assert waits == [1.0, 7.0]
    assert len(requests) == 3


def test_persistent_bridge_rate_limit_exhausts_admission_without_early_retry(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    clock = [0.0]
    attempts: list[float] = []

    def request(*_: object) -> object:
        attempts.append(clock[0])
        raise sonar_ide_analysis.BridgeRateLimited('IDE bridge returned HTTP 429', 4.0)

    def wait(delay: float) -> bool:
        clock[0] += delay
        return False

    monkeypatch.setattr(sonar_ide_analysis, 'request_bridge', request)
    monkeypatch.setattr(sonar_ide_analysis.time, 'monotonic', lambda: clock[0])
    monkeypatch.setattr(sonar_ide_analysis.CANCELLED, 'wait', wait)
    with pytest.raises(sonar_ide_analysis.BridgeRateLimited, match='429'):
        sonar_ide_analysis.analyze_when_indexed(64120, [tmp_path / 'source.cpp'], 30, 10)
    assert attempts == [0.0, 4.0, 8.0]


@pytest.mark.parametrize('status', [401, 403, 500, 503])
def test_other_bridge_failures_are_not_retried(status: int, tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    def request(*_: object, **__: object) -> object:
        raise sonar_ide_analysis.HTTPError('http://127.0.0.1:64120', status, 'failed', None, io.BytesIO(b'failure'))

    def unexpected_wait(_: float) -> bool:
        pytest.fail('authentication and unrelated server errors must fail immediately')

    monkeypatch.setattr(sonar_ide_analysis, 'urlopen', request)
    monkeypatch.setattr(sonar_ide_analysis.CANCELLED, 'wait', unexpected_wait)
    with pytest.raises(sonar_ide_analysis.AnalysisError, match=f'HTTP {status}'):
        sonar_ide_analysis.analyze_when_indexed(64120, [tmp_path / 'source.cpp'], 30, 10)


@pytest.mark.parametrize(('header', 'delay'), [(None, 5.0), ('0', 0.0), ('12', 12.0), ('-1', 5.0),
                                             ('nan', 5.0), ('inf', 5.0), ('invalid', 5.0)])
def test_bridge_retry_after_is_finite(header: str | None, delay: float) -> None:
    assert sonar_ide_analysis.bridge_retry_after(header) == delay


def test_bridge_retry_after_honors_http_date(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(sonar_ide_analysis.time, 'time', lambda: 1000.0)
    assert sonar_ide_analysis.bridge_retry_after('Thu, 01 Jan 1970 00:17:00 GMT') == 20.0


def test_bridge_retry_wait_can_be_cancelled(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    def request(*_: object) -> object:
        raise sonar_ide_analysis.BridgeRateLimited('IDE bridge returned HTTP 429', 5.0)

    monkeypatch.setattr(sonar_ide_analysis, 'request_bridge', request)
    monkeypatch.setattr(sonar_ide_analysis.CANCELLED, 'wait', lambda _: True)
    with pytest.raises(sonar_ide_analysis.AnalysisError, match='cancelled'):
        sonar_ide_analysis.analyze_when_indexed(64120, [tmp_path / 'source.cpp'], 30, 10)


@pytest.mark.parametrize('status', [401, 403, 503])
def test_indexing_text_cannot_make_other_http_failures_retry(status: int, tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    def request(*_: object) -> object:
        raise sonar_ide_analysis.AnalysisError(
            f'IDE bridge returned HTTP {status}: No files were found to be indexed by SonarQube for IDE')

    monkeypatch.setattr(sonar_ide_analysis, 'request_bridge', request)
    with pytest.raises(sonar_ide_analysis.AnalysisError, match=f'HTTP {status}'):
        sonar_ide_analysis.analyze_when_indexed(64120, [tmp_path / 'source.cpp'], 30, 10)


def test_bridge_does_not_retry_after_a_delayed_wakeup(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    clock = [0.0]
    attempts: list[float] = []

    def request(*_: object) -> object:
        attempts.append(clock[0])
        raise sonar_ide_analysis.BridgeRateLimited('IDE bridge returned HTTP 429', 1.0)

    def wait(_: float) -> bool:
        clock[0] = 20.0
        return False

    monkeypatch.setattr(sonar_ide_analysis, 'request_bridge', request)
    monkeypatch.setattr(sonar_ide_analysis.time, 'monotonic', lambda: clock[0])
    monkeypatch.setattr(sonar_ide_analysis.CANCELLED, 'wait', wait)
    with pytest.raises(sonar_ide_analysis.BridgeRateLimited, match='429'):
        sonar_ide_analysis.analyze_when_indexed(64120, [tmp_path / 'source.cpp'], 30, 10)
    assert attempts == [0.0]


@pytest.mark.parametrize(('value', 'expected'), [
    (r'C:\Project\MixedCase\Renderer.cpp', r'c:\Project\MixedCase\Renderer.cpp'),
    (r'c:\Project\MixedCase\Renderer.cpp', r'c:\Project\MixedCase\Renderer.cpp'),
    (r'\\Server\Share\MixedCase\Renderer.cpp', r'\\Server\Share\MixedCase\Renderer.cpp'),
    (r'\\?\C:\Project\Renderer.cpp', r'\\?\C:\Project\Renderer.cpp'),
    (r'C:Renderer.cpp', r'C:Renderer.cpp'),
])
def test_bridge_matches_windows_drive_uri_without_folding_other_names(value: str, expected: str,
                                                                     monkeypatch: pytest.MonkeyPatch) -> None:
    with monkeypatch.context() as patch:
        patch.setattr(sonar_ide_analysis.sys, 'platform', 'win32')
        result = sonar_ide_analysis.bridge_file_path(PureWindowsPath(value))
    assert result == expected


def test_bridge_does_not_change_paths_on_other_platforms(monkeypatch: pytest.MonkeyPatch) -> None:
    with monkeypatch.context() as patch:
        patch.setattr(sonar_ide_analysis.sys, 'platform', 'linux')
        result = sonar_ide_analysis.bridge_file_path(PureWindowsPath(r'C:\Project\Renderer.cpp'))
    assert result == r'C:\Project\Renderer.cpp'


def test_analysis_transport_sends_the_windows_index_identity(monkeypatch: pytest.MonkeyPatch) -> None:
    requested: list[str] = []

    def request(_port: int, _path: str, _timeout: float, body: bytes) -> object:
        requested.extend(json.loads(body)['fileAbsolutePaths'])
        return {'findings': [{'ruleKey': 'cpp:S1'}]}

    monkeypatch.setattr(sonar_ide_analysis, 'request_bridge', request)
    with monkeypatch.context() as patch:
        patch.setattr(sonar_ide_analysis.sys, 'platform', 'win32')
        result = sonar_ide_analysis.analyze_when_indexed(64120, [PureWindowsPath(r'C:\Project\Renderer.cpp')], 30, 10)
    assert requested == [r'c:\Project\Renderer.cpp']
    assert result == [{'ruleKey': 'cpp:S1'}]


def test_windows_completion_matches_the_index_uri_and_still_requires_each_file(monkeypatch: pytest.MonkeyPatch) -> None:
    source = PureWindowsPath(r'C:\Project\MixedCase\Renderer.cpp')
    header = PureWindowsPath(r'C:\Project\MixedCase\Renderer.h')
    log = 'file:///c:/Project/MixedCase/Renderer.cpp 456 cpp] 1 compilation units analyzed Analysis detected 1 issue'
    with monkeypatch.context() as patch:
        patch.setattr(sonar_ide_analysis.sys, 'platform', 'win32')
        assert sonar_ide_analysis.verify_evidence(log, [source])['completion']
        with pytest.raises(sonar_ide_analysis.AnalysisError, match='all submitted files'):
            sonar_ide_analysis.verify_evidence(log, [source, header])
        with pytest.raises(sonar_ide_analysis.AnalysisError, match='analysis errors'):
            sonar_ide_analysis.verify_evidence(log + ' [Error - CFamily failed', [source])


@pytest.mark.parametrize('reported', ['/c:/Project/MixedCase/Renderer.cpp', '/C:/Project/MixedCase/Renderer.cpp'])
def test_windows_finding_paths_become_native_without_losing_the_original(reported: str, monkeypatch: pytest.MonkeyPatch) -> None:
    finding = {'ruleKey': 'cpp:S1238', 'filePath': reported, 'message': 'actual finding'}
    with monkeypatch.context() as patch:
        patch.setattr(sonar_ide_analysis.sys, 'platform', 'win32')
        patch.setattr(sonar_ide_analysis, 'Path', PureWindowsPath)
        result = sonar_ide_analysis.mapped_findings([finding], PureWindowsPath(r'C:\Project'))
    assert result == [{**finding, 'filePath': str(PureWindowsPath(reported[1:])), 'reportedFilePath': reported}]
    assert finding['filePath'] == reported


def test_posix_finding_paths_are_not_reinterpreted_as_windows(monkeypatch: pytest.MonkeyPatch) -> None:
    finding = {'ruleKey': 'cpp:S1', 'filePath': '/c:/Project/Renderer.cpp'}
    with monkeypatch.context() as patch:
        patch.setattr(sonar_ide_analysis.sys, 'platform', 'linux')
        patch.setattr(sonar_ide_analysis, 'Path', PurePosixPath)
        patch.setattr(sonar_ide_analysis, 'original_header', lambda candidate, _: candidate)
        result = sonar_ide_analysis.mapped_findings([finding], PurePosixPath('/repo'))
    assert result == [finding]


def test_windows_findings_in_selected_files_cannot_be_misclassified_as_context(monkeypatch: pytest.MonkeyPatch) -> None:
    finding = {'ruleKey': 'cpp:S1238', 'filePath': r'c:\Project\MixedCase\Renderer.cpp', 'message': 'selected'}
    context = {'ruleKey': 'cpp:S1', 'filePath': r'c:\Project\Elsewhere.cpp', 'message': 'retain context'}
    with monkeypatch.context() as patch:
        patch.setattr(sonar_ide_analysis.sys, 'platform', 'win32')
        patch.setattr(sonar_ide_analysis, 'Path', PureWindowsPath)
        selected, contextual = sonar_ide_analysis.partition_sonar_findings(
            [finding, dict(finding), context], [PureWindowsPath(r'C:\Project\MixedCase\Renderer.cpp')])
    assert selected == [finding]
    assert contextual == [context]


def test_posix_selected_file_matching_preserves_case_and_unknown_paths(monkeypatch: pytest.MonkeyPatch) -> None:
    selected_finding = {'filePath': '/repo/Renderer.cpp', 'ruleKey': 'cpp:S1'}
    other_case = {'filePath': '/repo/renderer.cpp', 'ruleKey': 'cpp:S2'}
    no_path = {'ruleKey': 'cpp:S3'}
    with monkeypatch.context() as patch:
        patch.setattr(sonar_ide_analysis.sys, 'platform', 'linux')
        patch.setattr(sonar_ide_analysis, 'Path', PurePosixPath)
        selected, contextual = sonar_ide_analysis.partition_sonar_findings(
            [selected_finding, other_case, no_path], [PurePosixPath('/repo/Renderer.cpp')])
    assert selected == [selected_finding]
    assert contextual == [other_case, no_path]


def test_filter_findings_to_ranges_suppresses_unchanged_lines(tmp_path: Path) -> None:
    source = tmp_path / "source.cpp"
    findings = [
        {"filePath": str(source), "textRange": {"startLine": 4, "endLine": 4}},
        {"filePath": str(source), "textRange": {"startLine": 12, "endLine": 13}},
    ]

    selected, suppressed = sonar_ide_analysis.filter_findings_to_ranges(findings, {source: [(10, 14)]})

    assert selected == [findings[1]]
    assert suppressed == 1


def test_legacy_payload_cannot_claim_clean_without_sensor_evidence() -> None:
    assert not hasattr(sonar_ide_analysis, "result_payload")


def test_run_git_translates_process_failures(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(sonar_ide_analysis, 'resolved_tool', lambda _: "/tools/git")

    def fail(*_args: object, **_kwargs: object) -> object:
        raise sonar_ide_analysis.subprocess.CalledProcessError(1, "git", stderr=b"bad ref")

    monkeypatch.setattr(sonar_ide_analysis.subprocess, 'run', fail)
    with pytest.raises(sonar_ide_analysis.AnalysisError, match="bad ref"):
        sonar_ide_analysis.run_git(tmp_path, "status")


def test_run_git_uses_a_literal_argv0_and_resolved_executable(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    calls: list[tuple[object, dict[str, object]]] = []
    monkeypatch.setattr(sonar_ide_analysis, 'resolved_tool', lambda _: "/tools/git")

    def run(command: object, **kwargs: object) -> SimpleNamespace:
        calls.append((command, kwargs))
        return SimpleNamespace(returncode=0, stdout=b"ok", stderr=b"")

    monkeypatch.setattr(sonar_ide_analysis.subprocess, 'run', run)

    assert sonar_ide_analysis.run_git(tmp_path, "status") == b"ok"
    assert calls == [
        (
            ["git", "status"],
            {"executable": "/tools/git", "cwd": tmp_path.resolve(), "check": True, "capture_output": True, "shell": False},
        )
    ]


def test_main_rejects_combining_explicit_files_with_a_change_selector(capsys: pytest.CaptureFixture[str]) -> None:
    with pytest.raises(SystemExit) as failure:
        sonar_ide_analysis.main(["check", "--base", "origin/main", "--files", "src/example.cpp"])
    assert failure.value.code == 2
    assert "not allowed with argument" in capsys.readouterr().err


def test_main_rejects_unverified_explicit_port(capsys: pytest.CaptureFixture[str]) -> None:
    with pytest.raises(SystemExit) as failure:
        sonar_ide_analysis.main(["check", "--port", "64120", "--files", "source.cpp"])
    assert failure.value.code == 2
    assert "unrecognized arguments: --port" in capsys.readouterr().err


def test_run_git_keeps_option_like_worktree_names_out_of_argv(tmp_path, monkeypatch):
    root = tmp_path / "--exec=hostile directory örnek"
    root.mkdir()
    calls = []
    monkeypatch.setattr(sonar_ide_analysis, 'resolved_tool', lambda _: "/tools/git")

    def run(command, **kwargs):
        calls.append((command, kwargs))
        return SimpleNamespace(stdout=b"ok")
    monkeypatch.setattr(sonar_ide_analysis.subprocess, 'run', run)
    assert sonar_ide_analysis.run_git(root, "status") == b"ok"
    assert calls[0][0] == ["git", "status"]
    assert calls[0][1]["cwd"] == root.resolve()


@pytest.mark.parametrize("generator", ["Ninja", "Unix Makefiles"])
def test_prepare_keeps_compiler_context_builds_serial(tmp_path, monkeypatch, generator):
    source = tmp_path / "input.cpp"
    source.write_text("int value = 1;\n", encoding="utf-8")
    build = tmp_path / "build"
    build.mkdir()
    (build / "CMakeCache.txt").write_text(
        f"CMAKE_HOME_DIRECTORY:INTERNAL={tmp_path}\nCMAKE_GENERATOR:INTERNAL={generator}\n", encoding="utf-8")
    database = build / "compile_commands.json"
    database.write_text(json.dumps([{"directory": str(build), "file": str(source),
        "arguments": ["c++", "-c", str(source), "-o", "CMakeFiles/fixture.dir/input.cpp.obj"]}]), encoding="utf-8")
    calls = []

    def run(command, *args):
        calls.append(command)
        return SimpleNamespace(returncode=0, stdout="", stderr="")

    monkeypatch.setattr(sonar_ide_analysis, "quality_run", run)
    monkeypatch.setenv("CMAKE_BUILD_PARALLEL_LEVEL", "8")
    result, contexts = sonar_ide_analysis.prepare(tmp_path, tmp_path / "cache", [source], database, 10)
    assert result == database
    assert contexts == {}
    build_commands = [command for command in calls if "--build" in command]
    assert len(build_commands) == 1
    assert build_commands[0][build_commands[0].index("--parallel") + 1] == "1"
