"""Regression coverage for scope, ownership, incomplete scans and local metrics."""

from __future__ import annotations

import concurrent.futures
import json
import shutil
import sys
import threading
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import quality_preflight as preflight
import quality_support as support
import quality_tools as tools
import sonar_preflight as sonar
import sonar_sessions as sessions
from sonar_ide_analysis import AnalysisError


def git(root: Path, *args: str) -> str:
    executable = shutil.which("git")
    assert executable is not None
    # Arguments name only disposable fixture repositories; shell execution is disabled.
    process = support.run([executable, "-C", str(root), *args], root, 30)
    assert process.returncode == 0, process.stderr
    return process.stdout.strip()


@pytest.fixture
def repository(tmp_path: Path) -> Path:
    root = tmp_path / "repo with boşluk"
    root.mkdir()
    git(root, "init", "-q")
    git(root, "config", "user.name", "Fixture")
    git(root, "config", "user.email", "fixture@example.invalid")
    (root / "old name.cpp").write_text("int value;\n")
    (root / "removed.cpp").write_text("int removed;\n")
    git(root, "add", ".")
    git(root, "commit", "-qm", "fixture")
    git(root, "update-ref", "refs/remotes/origin/main", "HEAD")
    return root


def test_branch_scope_includes_committed_staged_unstaged_untracked_and_rename(repository: Path):
    git(repository, "mv", "old name.cpp", "yeni ü.cpp")
    git(repository, "rm", "removed.cpp")
    git(repository, "commit", "-qm", "fixture delta")
    source = repository / "yeni ü.cpp"
    source.write_text("int value;\nint staged;\n")
    git(repository, "add", ".")
    source.write_text("int value;\nint staged;\nint unstaged;\n")
    (repository / "untracked.hpp").write_text("int newValue;\n")
    args = preflight.arguments(["check", "--worktree", str(repository)])
    selected, skipped, _, base = preflight.selection(repository, args)
    assert set(map(str, selected)) == {"yeni ü.cpp", "untracked.hpp"}
    assert base == git(repository, "rev-parse", "origin/main")
    assert not skipped
    ranges = support.changed_ranges(repository, base, selected)
    assert ranges["untracked.hpp"] == [(1, 1)]
    assert ranges["yeni ü.cpp"]


def test_dirty_scope_handles_unstaged_delete_and_spaces(repository: Path):
    (repository / "removed.cpp").unlink()
    (repository / "new space.cpp").write_text("int n;\n")
    args = preflight.arguments(["check", "--dirty"])
    files, skipped, source, _ = preflight.selection(repository, args)
    assert files == [Path("new space.cpp")]
    assert source == "dirty"
    assert skipped == [{"path": "removed.cpp", "reason": "deleted_or_missing", "intentional": True}]


def test_missing_base_fails_without_fetch(repository: Path):
    with pytest.raises(AnalysisError):
        preflight.selection(repository, preflight.arguments(["check", "--base", "origin/missing"]))


def test_snapshot_does_not_share_inodes_or_write_source(repository: Path, tmp_path: Path):
    mirror = tools.snapshot(repository, tmp_path / "mirror")
    (mirror / "old name.cpp").write_text("modified by tool")
    assert (repository / "old name.cpp").read_text() == "int value;\n"
    tools.snapshot(repository, mirror)
    assert (mirror / "old name.cpp").read_text() == "int value;\n"
    (repository / "removed.cpp").unlink()
    tools.snapshot(repository, mirror)
    assert not (mirror / "removed.cpp").exists()
    assert git(mirror, "ls-files")


def test_fingerprint_detects_header_changes_and_new_files(repository: Path):
    before = support.fingerprint(repository)
    (repository / "new.hpp").write_text("int first;\n")
    after = support.fingerprint(repository)
    assert before != after
    (repository / "new.hpp").write_text("int other;\n")
    assert after != support.fingerprint(repository)


def test_changed_line_annotation_keeps_context_and_unknown_ranges():
    findings = [{"filePath": "a.cpp", "line": 2, "message": "context"},
                {"filePath": "a.cpp", "line": 9, "endLine": 12},
                {"filePath": "a.cpp", "message": "file-level"}]
    result = preflight.annotate(findings, Path("/repo"), {"a.cpp": [(10, 11)]})
    assert len(result) == 3
    assert result[0]["changedLine"] is True
    assert {item["changedLine"] for item in result} == {True, False, None}


def test_ownership_rejects_pid_reuse_and_substring_profile():
    record = {"pid": 123, "started": "77", "profile": "/cache/unique/profile", "nonce": "session",
              "executable": "/usr/share/code/code"}
    table = {123: {"started": "77", "parent": 1, "argv": ["code", "--user-data-dir", record["profile"]],
                   "executable": record["executable"],
                   "markers": {"HORO_QUALITY_SESSION": "session", "HORO_QUALITY_PROFILE": record["profile"]}}}
    assert sessions.owned(record, table)
    table[123]["started"] = "78"
    assert not sessions.owned(record, table)
    table[123]["started"] = "77"
    table[123]["argv"][-1] += "-unrelated"
    table[123]["markers"]["HORO_QUALITY_PROFILE"] += "-unrelated"
    assert not sessions.owned(record, table)


def test_terminate_never_signals_stale_or_unrelated_process(monkeypatch):
    monkeypatch.setattr(sessions, "processes", lambda: {123: {"started": "new", "argv": ["code"]}})
    monkeypatch.setattr(sessions.os, "pidfd_open", lambda _: pytest.fail("must not signal unowned process"))
    assert not sessions.terminate({"pid": 123, "started": "old", "profile": "/cache/profile"})


def test_third_session_waits_for_one_of_two_leases(tmp_path: Path):
    acquired = threading.Event()
    pools = [sessions.SessionPool(tmp_path, tmp_path / str(index), 3) for index in range(3)]

    def acquire_third():
        with pools[2].lease():
            acquired.set()
    with concurrent.futures.ThreadPoolExecutor() as executor:
        with pools[0].lease():
            with pools[1].lease():
                task = executor.submit(acquire_third)
                assert not acquired.wait(0.15)
            assert acquired.wait(2)
        task.result()


def test_pool_queue_timeout_and_stop_busy_session(tmp_path: Path):
    pool = sessions.SessionPool(tmp_path, tmp_path / "root", 0.05)
    with pool.lease():
        assert pool.stop() == [{"slot": 0, "status": "busy"}]
        with pool.lease():
            with pytest.raises(AnalysisError, match="two Sonar sessions"):
                with pool.lease():
                    pytest.fail("third lease unexpectedly acquired")


def test_pool_releases_lease_after_failure(tmp_path: Path):
    pool = sessions.SessionPool(tmp_path, tmp_path / "root", 0.1)
    with pytest.raises(RuntimeError):
        with pool.lease():
            raise RuntimeError("cancel")
    with pool.lease():
        pass


def test_cpp_evidence_rejects_empty_issue_list_without_sensor():
    source = Path("/repo/source.cpp")
    with pytest.raises(AnalysisError, match="No positive"):
        sonar.verify_evidence(source.as_uri() + " Analysis detected 0 issues 0 compilation units analyzed", [source])


@pytest.mark.parametrize("suffix", ["[Error - invalid compile commands", "Failed to analyze files"])
def test_cpp_evidence_rejects_sensor_errors(suffix: str):
    source = Path("/repo/source.cpp")
    with pytest.raises(AnalysisError, match="analysis errors"):
        sonar.verify_evidence(f"{source.as_uri()} 456 cpp] 1 compilation units analyzed Analysis detected {suffix}", [source])


def test_cpp_evidence_requires_every_submitted_file():
    source = Path("/repo/source.cpp")
    log = f"{source.as_uri()} activeRules: [456 cpp] 1 compilation units analyzed Analysis detected 0 issues"
    assert sonar.verify_evidence(log, [source])["compilationUnits"] == 1
    with pytest.raises(AnalysisError, match="all submitted"):
        sonar.verify_evidence(log, [source, Path("/repo/header.hpp")])


def test_header_context_follows_transitive_literal_includes(tmp_path: Path):
    source, intermediate, header = [tmp_path / name for name in ("source.cpp", "intermediate.h", "header.h")]
    source.write_text('#include "intermediate.h"\n')
    intermediate.write_text('#include "header.h"\n')
    header.write_text("int value;\n")
    entry = {"source": source, "includes": [tmp_path], "target": "source.o"}
    assert sonar.header_context([entry], [header], tmp_path) == {header: entry}
    missing = tmp_path / "orphan.h"
    missing.write_text("int other;\n")
    with pytest.raises(AnalysisError, match="No translation unit"):
        sonar.header_context([entry], [missing], tmp_path)


def test_wrong_worktree_compilation_database_is_rejected(tmp_path: Path):
    build = tmp_path / "build"
    build.mkdir()
    database = build / "compile_commands.json"
    database.write_text("[]")
    (build / "CMakeCache.txt").write_text("CMAKE_HOME_DIRECTORY:INTERNAL=/another/worktree\n")
    with pytest.raises(AnalysisError, match="different worktree"):
        sonar.prepare(tmp_path, tmp_path / "cache", [], database, 2)


def test_compiler_entries_require_in_build_object_target(tmp_path: Path):
    database = tmp_path / "compile_commands.json"
    database.write_text(json.dumps([{"directory": str(tmp_path), "file": "source.cpp", "command": "c++ -c source.cpp"}]))
    with pytest.raises(AnalysisError, match="object target"):
        sonar.compiler_entries(database, tmp_path)


def test_duplication_retains_unchanged_clone_partner(tmp_path: Path):
    xml = f'''<pmd-cpd><duplication lines="5" tokens="80">
      <file path="{tmp_path}/changed.cpp" line="10"/>
      <file path="{tmp_path}/unchanged.cpp" line="20"/>
    </duplication></pmd-cpd>'''
    clones = tools.parse_clones(xml, tmp_path, {"changed.cpp"})
    assert clones[0]["occurrences"] == [{"path": "changed.cpp", "startLine": 10, "endLine": 14},
                                          {"path": "unchanged.cpp", "startLine": 20, "endLine": 24}]
    assert not tools.parse_clones(xml, tmp_path, {"unrelated.cpp"})


@pytest.mark.parametrize("xml", ['<pmd-cpd><error/></pmd-cpd>', '<!DOCTYPE a><pmd-cpd/>', '<unknown/>'])
def test_cpd_rejects_partial_or_unexpected_xml(xml: str, tmp_path: Path):
    with pytest.raises(AnalysisError):
        tools.parse_clones(xml, tmp_path, {"a.cpp"})


def test_codacy_zero_exit_with_missing_tool_is_incomplete(tmp_path: Path, monkeypatch):
    payload = {"issues": [], "errors": [], "toolResults": [], "capability": {"unavailable": [{"toolId": "Lizard"}]}}
    monkeypatch.setattr(tools, "json_command", lambda *_: (payload, 0))
    result = tools.codacy_check(tmp_path, tmp_path, [Path("a.cpp")], {"tools": [{"toolId": "Lizard"}]},
                                {"Lizard": {"languages": ["CPP"]}}, 2)
    assert result["status"] == "incomplete"
    assert result["errors"][0]["reason"] == "missing_or_incomplete_execution"


def test_codacy_routes_cpp_alias_to_semgrep(tmp_path: Path, monkeypatch):
    calls = []
    payload = {"issues": [], "errors": [], "toolResults": [{"toolId": "Semgrep", "status": "success", "filesAnalyzed": 1}]}
    monkeypatch.setattr(tools, "json_command", lambda argv, *_: (calls.append(argv) or payload, 0))
    result = tools.codacy_check(tmp_path, tmp_path, [Path("a.cpp")], {"tools": [{"toolId": "Semgrep"}]},
                                {"Semgrep": {"languages": ["C++"]}}, 2)
    assert result["status"] == "clean"
    assert "Semgrep" in calls[0]


def test_secrets_does_not_store_sensitive_scanner_output(tmp_path: Path, monkeypatch):
    monkeypatch.setattr(tools, "run", lambda *_: SimpleNamespace(returncode=1, stdout="secret-value", stderr=""))
    result = tools.secrets(tmp_path, [Path("a.cpp")], 2)
    assert result["status"] == "issues_found"
    assert "secret-value" not in json.dumps(result)


def test_guarded_provider_failure_keeps_forbidden_distinct():
    def forbidden():
        raise AnalysisError("Codacy cloud configuration access failed (403 Forbidden)")
    result = preflight.guarded(forbidden)
    assert result["status"] == "incomplete"
    assert "403" in result["error"]


@pytest.mark.parametrize("offline", [False, True])
def test_doctor_checks_configuration_without_sonar_analysis(tmp_path: Path, monkeypatch, offline):
    calls = []
    monkeypatch.setattr(preflight, "tool_versions", lambda *_: {})
    monkeypatch.setattr(tools, "codacy_info", lambda *_: {"status": "complete"})
    monkeypatch.setattr(preflight, "cloud_drift",
                        lambda *_: calls.append("cloud") or {"status": "matched"})

    def unexpected_analysis(*_):
        raise AssertionError("doctor must not select files or run Sonar analysis")

    monkeypatch.setattr(tools, "repository_files", unexpected_analysis)
    monkeypatch.setattr(tools, "json_command", unexpected_analysis)
    args = preflight.arguments(["doctor", "--worktree", str(tmp_path)] + (["--offline"] if offline else []))
    report = preflight.doctor(tmp_path, tmp_path / "cache", args)
    assert "vortex" not in report
    assert calls == ([] if offline else ["cloud"])
    assert report["cloudConfiguration"]["status"] == ("not_checked" if offline else "matched")


def test_two_real_worktrees_have_separate_cache_identity(repository: Path, tmp_path: Path, monkeypatch):
    other = tmp_path / "other worktree"
    git(repository, "worktree", "add", "--detach", str(other), "HEAD")
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "cache"))
    assert support.cache_directory(repository) == support.cache_directory(other)
    assert support.identity(repository) != support.identity(other)
    (other / "old name.cpp").write_text("int distinct;\n")
    assert support.fingerprint(repository) != support.fingerprint(other)


def test_input_changes_force_incomplete_even_if_providers_clean(repository: Path, tmp_path: Path, monkeypatch):
    args = preflight.arguments(["check", "--only", "codacy", "--files", "old name.cpp"])

    def analyzer(*_):
        (repository / "old name.cpp").write_text("int modified;\n")
        return {"status": "clean", "issues": []}
    monkeypatch.setattr(preflight, "codacy_stage", analyzer)
    result = preflight.check(repository, tmp_path / "cache", args)
    assert result["stale"]
    assert result["exitCode"] == 2


def test_owned_command_timeout_cleans_up(tmp_path: Path):
    with pytest.raises(AnalysisError, match="timed out"):
        support.run([sys.executable, "-c", "import time; time.sleep(5)"], tmp_path, 0.05)


def test_log_rotation_keeps_only_current_request(tmp_path: Path):
    directory = tmp_path / "logs/20261002/window1/Sonar"
    directory.mkdir(parents=True)
    original = directory / "SonarQube for IDE.log"
    original.write_text("prior request\n")
    offsets = sessions.log_offsets(tmp_path)
    rotated = directory / "SonarQube for IDE.1.log"
    original.rename(rotated)
    with rotated.open("a") as stream:
        stream.write("current sensor\n")
    original.write_text("current completion\n")
    text = sessions.log_delta(tmp_path, offsets)
    assert "prior request" not in text
    assert "current sensor" in text and "current completion" in text


def test_indexing_retry_does_not_hide_completed_sensor_error():
    source = Path("/repo/source.cpp")
    retry = "[Error - Failed to analyze files\nNo files were found to be indexed by SonarQube for IDE\n"
    success = f"Analyzing list of 1 files: {source.as_uri()} 456 cpp] 1 compilation units analyzed Analysis detected"
    assert sonar.verify_evidence(retry + success, [source])["completion"]
    with pytest.raises(AnalysisError, match="analysis errors"):
        sonar.verify_evidence(retry + success + " [Error - sensor failure", [source])


def test_live_unverified_session_is_not_evicted(tmp_path: Path, monkeypatch):
    record = {"pid": 99, "started": "123"}
    monkeypatch.setattr(sessions, "terminate", lambda _: False)
    monkeypatch.setattr(sessions, "processes", lambda: {99: {"started": "123"}})
    with pytest.raises(AnalysisError, match="slot remains reserved"):
        sessions.retire(record)
    pool = sessions.SessionPool(tmp_path, tmp_path / "root")
    support.write_json(pool.directory / "0.json", record)
    assert pool.stop()[0]["status"] == "unverified"
    assert (pool.directory / "0.json").exists()


def test_binding_ignores_previous_server_logs(tmp_path: Path):
    message = ("isReadyForAnalysis(connectionId: test, sonarProjectKey: project, "
               "plugins: true, analyzer config: true, findings: true) => true")
    old = tmp_path / "logs/20261001/SonarQube for IDE.log"
    new = tmp_path / "logs/20261002/SonarQube for IDE.log"
    old.parent.mkdir(parents=True)
    new.parent.mkdir(parents=True)
    old.write_text(message)
    new.write_text(message.replace("=> true", "=> false"))
    assert not sonar.binding_evidence(tmp_path, "project")
    with new.open("a") as stream:
        stream.write("\n" + message)
    assert sonar.binding_evidence(tmp_path, "project")


def test_cancelled_session_queue_releases_registry(tmp_path: Path):
    pool = sessions.SessionPool(tmp_path, tmp_path / "root", 0.1)
    support.CANCELLED.set()
    try:
        with pytest.raises(AnalysisError, match="cancelled"):
            with pool.lease():
                pytest.fail("cancelled queue acquired session")
    finally:
        support.CANCELLED.clear()
    with pool.lease():
        pass


def test_worktree_branch_warning_preserves_sensor_proof():
    source = Path("/repo/source.cpp")
    log = (f"[Error - Couldn't access repository for path /repo\nAnalyzing list of files {source.as_uri()} "
           "activeRules: [456 cpp] 1 compilation units analyzed Analysis detected 0 issues")
    assert sonar.verify_evidence(log, [source])["branchMatching"] == "unavailable"
    with pytest.raises(AnalysisError, match="analysis errors"):
        sonar.verify_evidence(log + " [Error - CFamily failed", [source])


def test_staged_public_header_uses_original_translation_unit(tmp_path: Path):
    root = tmp_path / "root"
    original = root / "include/Horo/Value.h"
    staged = tmp_path / "external/target-includes/Value/public/Horo/Value.h"
    source = root / "src/source.cpp"
    for path in (original, staged, source):
        path.parent.mkdir(parents=True, exist_ok=True)
    original.write_text("struct Value {};\n")
    staged.write_text(original.read_text())
    source.write_text('#include <Horo/Value.h>\n')
    entry = {"source": source, "includes": [staged.parents[1]], "target": "source.o"}
    assert sonar.header_context([entry], [original], root)[original] == entry
    staged.write_text("stale copy\n")
    with pytest.raises(AnalysisError, match="No translation unit"):
        sonar.header_context([entry], [original], root)


def test_partial_codacy_file_execution_is_incomplete():
    payload = {"toolResults": [{"toolId": "tool", "status": "success", "filesAnalyzed": 1}],
               "capability": {"ready": [{"toolId": "tool", "filesRouted": 2}]}}
    assert any(item["reason"] == "routed_file_count_mismatch"
               for item in tools.execution_errors(payload, ["tool"]))


def test_private_profile_preserves_user_settings_and_credentials(tmp_path: Path):
    import sqlite3

    source = tmp_path / "user"
    settings = source / "User/settings.json"
    settings.parent.mkdir(parents=True)
    dummy_credential = "fixture-" + tmp_path.name
    original = {"editor.fontSize": 18, "security.workspace.trust.enabled": True,
                "sonarlint.connectedMode.connections.sonarcloud":
                [{"connectionId": "test", "organizationKey": "org", "token": dummy_credential}]}
    support.write_json(settings, original)
    database = source / "User/globalStorage/state.vscdb"
    database.parent.mkdir()
    sonar_key = 'secret://{"extensionId":"sonarsource.sonarlint-vscode","key":"connection"}'
    other_key = 'secret://{"extensionId":"unrelated","key":"connection"}'
    with sqlite3.connect(database) as connection:
        connection.execute("CREATE TABLE ItemTable (key TEXT, value BLOB)")
        connection.executemany("INSERT INTO ItemTable VALUES (?,?)",
                               [(sonar_key, "encrypted-fixture"), (other_key, "private-fixture"),
                                ("content.trust.model.key", '{"uriTrustInfo": []}')])
    before = database.read_bytes()
    extensions = tmp_path / "extensions/sonarsource.sonarlint-vscode-fixture"
    extensions.mkdir(parents=True)
    support.write_json(extensions / "package.json", {"version": "fixture"})
    profile = tmp_path / "private/profile"
    sessions.seed_profile(profile, source, extensions.parent)
    assert support.read_json(settings) == original and database.read_bytes() == before
    copied = support.read_json(profile / "User/settings.json")
    assert copied["security.workspace.trust.enabled"] is False
    assert "workspace.trust.enabled" not in copied
    assert "token" not in copied["sonarlint.connectedMode.connections.sonarcloud"][0]
    assert "editor.fontSize" not in copied and "-Duser.home=" in copied["sonarlint.ls.vmargs"]
    with sqlite3.connect(profile / "User/globalStorage/state.vscdb") as connection:
        assert connection.execute("SELECT key FROM ItemTable").fetchall() == [(sonar_key,)]
