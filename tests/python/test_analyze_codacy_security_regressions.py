"""Security boundaries in the consolidated analysis CLI."""
from pathlib import Path
import threading

import analyze
import pytest
from defusedxml.common import DefusedXmlException


def test_shared_xml_parser_rejects_entity_expansion():
    payload = '<!DOCTYPE coverage [<!ENTITY injected "expanded">]><coverage>&injected;</coverage>'
    with pytest.raises(DefusedXmlException):
        analyze.element_tree.fromstring(payload)


def test_bare_command_cannot_trust_relative_search_directories(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    executable = tmp_path / ("untrusted.cmd" if analyze.sys.platform == "win32" else "untrusted")
    executable.write_text("@echo off\n" if analyze.sys.platform == "win32" else "#!/bin/sh\n")
    executable.chmod(0o700)
    with pytest.raises(analyze.AnalysisError, match="absolute PATH"):
        analyze._command(["untrusted"], {"PATH": "."})


def test_native_bindings_have_separate_lock_and_job_operations():
    native_type = analyze.WindowsNative
    assert callable(native_type.try_lock)
    assert callable(native_type.attach_and_resume)
    assert callable(native_type.kill_job)


def test_failed_job_cleanup_still_drains_process_before_reporting_ambiguity():
    events = []
    class Native:
        def kill_job(self, job):
            events.append("kill_job")
            raise OSError("job drain denied")
        def close(self, job):
            events.append("close_job")
    class Handle:
        def Close(self):
            events.append("close_handle")
    class Process:
        stdout = stderr = None
        _handle = Handle()
        def poll(self):
            return None
        def kill(self):
            events.append("kill_leader")
        def communicate(self, timeout):
            events.append("drain")
    lifecycle = {"outcome": "ambiguous"}
    with pytest.raises(analyze.AnalysisError, match="cleanup failed"):
        analyze._finish_command(Native(), object(), Process(), False, lifecycle)
    assert events == ["kill_job", "close_job", "kill_leader", "drain", "close_handle"]
    assert lifecycle["outcome"] == "ambiguous"


def test_executable_disappearing_during_resolution_is_a_safe_launch_error(tmp_path, monkeypatch):
    monkeypatch.setattr(analyze.shutil, "which", lambda *args, **kwargs: str(tmp_path / "missing.exe"))
    lifecycle = {}
    with pytest.raises(analyze.AnalysisError, match="executable"):
        analyze.native_run([str(tmp_path / "missing.exe")], tmp_path, 1, None, threading.Event(), lifecycle)
    assert lifecycle["outcome"] == "no_escape"


def test_linux_listener_table_rejects_malformed_ownership_evidence(monkeypatch):
    monkeypatch.setattr(Path, "read_text", lambda *args, **kwargs: "header\nmalformed\n")
    with pytest.raises(analyze.AnalysisError, match="/proc/net"):
        analyze._linux_listening_sockets(range(64120, 64131))


def test_typed_launch_configuration_registers_before_both_commands(tmp_path, monkeypatch):
    extension = tmp_path / "extension"
    events = []
    lifecycle = {}
    configuration = analyze.LaunchConfiguration(
        timeout=1, activation_file=tmp_path / "activate.cpp",
        register=lambda nonce, path: events.append(("registered", nonce, path)), lifecycle=lifecycle)
    monkeypatch.setattr(analyze, "seed_profile", lambda *args: extension)
    def run(argv, root, timeout, env, attempt):
        assert events[0][0] == "registered"
        assert env["HORO_QUALITY_SESSION"] == events[0][1]
        assert "VSCODE_IPC_HOOK_CLI" not in env
        events.append(("command", argv))
        attempt["outcome"] = "no_escape"
    monkeypatch.setattr(analyze, "quality_run", run)
    nonce, selected = analyze.launch_profile(tmp_path, tmp_path / "profile", tmp_path / "workspace",
                                            tmp_path / "source", tmp_path / "extensions", configuration)
    assert selected == extension and nonce == events[0][1]
    assert len(events) == 3
    assert lifecycle["outcome"] == "no_escape"


def test_session_reuse_rejects_old_isolation_before_contacting_bridge(tmp_path, monkeypatch):
    pool = analyze.SessionPool(tmp_path, tmp_path / "root")
    monkeypatch.setattr(analyze, "session_processes", lambda: {})
    monkeypatch.setattr(analyze, "request_bridge", lambda *args: pytest.fail("unverified bridge"))
    assert not pool._reusable_session({"isolationVersion": 3}, tmp_path / "profile", tmp_path / "extensions")


@pytest.mark.parametrize("data", [[118, 49, 48, True], [256], [-1], ["118"], "v10"])
def test_secretstorage_bytes_rejects_non_byte_json_values(data):
    with pytest.raises(ValueError, match="envelope"):
        analyze._secretstorage_bytes({"type": "Buffer", "data": data})


def test_polling_cancellation_does_not_wait_for_output(tmp_path):
    cancelled = threading.Event()
    cancelled.set()
    class Process:
        def communicate(self, **kwargs):
            pytest.fail("cancelled command must be cleaned up, not waited on")
    with pytest.raises(analyze.AnalysisError, match="cancelled"):
        analyze._wait_command(Process(), "fixture", 1, cancelled)


def test_command_ownership_releases_successful_job_before_draining_pipes():
    events = []
    class Native:
        def create_job(self):
            events.append("create")
            return "job"
        def release_job(self, job):
            assert job == "job"
            events.append("release")
        def close(self, job):
            events.append("close")
    class Handle:
        def Close(self):
            events.append("handle")
    class Process:
        stdout = stderr = None
        _handle = Handle()
        def poll(self):
            return 0
        def communicate(self, **kwargs):
            events.append("drain")
    lifecycle = {"outcome": "no_escape"}
    with analyze.CommandOwnership(Native(), lifecycle) as command:
        assert command.options == {"creationflags": 4, "encoding": "utf-8", "errors": "replace"}
        assert lifecycle["outcome"] == "ambiguous"
        command.process = Process()
        command.completed = True
    assert events == ["create", "release", "close", "drain", "handle"]
    assert lifecycle["outcome"] == "ambiguous"


def test_interrupted_containment_entry_closes_the_owned_job():
    events = []
    class Native:
        def create_job(self):
            return "job"
        def close(self, job):
            events.append(("close", job))
    class Lifecycle(dict):
        def __setitem__(self, key, value):
            raise KeyboardInterrupt
    with pytest.raises(KeyboardInterrupt):
        with analyze.CommandOwnership(Native(), Lifecycle()):
            pytest.fail("interrupted ownership cannot launch")
    assert events == [("close", "job")]
