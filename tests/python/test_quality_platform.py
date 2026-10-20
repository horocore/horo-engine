"""Native regression tests for quality process and lease ownership."""
from __future__ import annotations

import analyze

import importlib
import os
from pathlib import Path
import sys
import threading

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))


def platform():
    return importlib.import_module('analyze')


def test_missing_psutil_points_to_current_requirements(monkeypatch):
    api = platform()
    monkeypatch.setitem(sys.modules, 'psutil', None)
    with pytest.raises(api.AnalysisError) as failure:
        api._psutil()
    assert "scripts/requirements.txt" in str(failure.value)
    assert "requirements-quality-windows" not in str(failure.value)


def test_native_lock_uses_one_stable_byte_even_after_append(tmp_path):
    api = platform()
    path = tmp_path / "lease"
    with path.open("a+") as first, path.open("a+") as second:
        first.write("existing metadata")
        first.flush()
        assert api.try_lock(first)
        try:
            second.seek(0, 2)
            assert not api.try_lock(second)
        finally:
            api.unlock(first)
        assert api.try_lock(second)
        api.unlock(second)


def test_native_paths_and_support(monkeypatch, tmp_path):
    api = platform()
    api.ensure_supported()
    if sys.platform == "win32":
        monkeypatch.setenv("LOCALAPPDATA", str(tmp_path / "local"))
        monkeypatch.setenv("APPDATA", str(tmp_path / "roaming"))
        assert api.cache_home() == tmp_path / "local/horo-quality"
        assert api.default_code_user_data() == tmp_path / "roaming/Code"
    else:
        monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path))
        monkeypatch.setenv("XDG_CONFIG_HOME", str(tmp_path / "config"))
        assert api.cache_home() == tmp_path / "horo-quality"
        assert api.default_code_user_data() == tmp_path / "config/Code"


def test_command_line_roundtrip_preserves_windows_paths():
    import subprocess
    api = platform()
    args = [r"C:\Program Files\Compiler\cl.exe", r"/IC:\some dir\ü" + "\\", "plain", ""]
    command = subprocess.list2cmdline(args) if sys.platform == "win32" else __import__("shlex").join(args)
    assert api.command_line_arguments(command) == args


def test_native_command_preserves_argv_and_environment(tmp_path):
    import json
    api = platform()
    arguments = ["space ü", "&echo injected", "%PATH%", 'quote"value', "", "trail\\"]
    env = dict(os.environ, HORO_PLATFORM_TEST="isolated")
    result = api.native_run([sys.executable, "-c", "import json,os,sys; print(json.dumps([sys.argv[1:],os.environ['HORO_PLATFORM_TEST']]))", *arguments],
                     tmp_path, 5, env, threading.Event())
    assert result.returncode == 0
    assert json.loads(result.stdout) == [arguments, "isolated"]


@pytest.mark.parametrize("cancel", [False, True])
@pytest.mark.parametrize("leader_exits", [False, True])
def test_timeout_and_cancel_remove_entire_owned_tree(tmp_path, cancel, leader_exits):
    import concurrent.futures
    import json
    import time
    import psutil
    AnalysisError = platform().AnalysisError
    api = platform()
    marker = tmp_path / "tree.json"
    pending_marker = marker.with_suffix(".tmp")
    child = "import time; time.sleep(60)"
    # Existence signals readiness only after the complete JSON is closed.
    source = ("import json,os,subprocess,sys,time; from pathlib import Path; "
              f"p=subprocess.Popen([sys.executable,'-c',{child!r}]); "
              f"Path({str(pending_marker)!r}).write_text(json.dumps([os.getpid(),p.pid]), encoding='utf-8'); "
              f"os.replace({str(pending_marker)!r},{str(marker)!r}); "
              f"time.sleep({0.5 if leader_exits else 60})")
    cancelled = threading.Event()
    with concurrent.futures.ThreadPoolExecutor() as executor:
        task = executor.submit(api.native_run, [sys.executable, "-c", source], tmp_path, 60 if cancel else 1.5, None, cancelled)
        deadline = time.monotonic() + 5
        while not marker.exists() and not task.done() and time.monotonic() < deadline:
            time.sleep(0.01)
        assert marker.exists(), "child did not start"
        pids = json.loads(marker.read_text())
        # Pin before cleanup: Windows can retain a terminated process object
        # (and its PID) while exception tracebacks/third parties hold handles.
        handles = [api._windows().open_process(pid) for pid in pids] if sys.platform == "win32" else []
        if cancel:
            cancelled.set()
        with pytest.raises(AnalysisError, match="cancelled|timed out"):
            task.result(timeout=10)
    if handles:
        import ctypes
        wait = ctypes.WinDLL("kernel32").WaitForSingleObject
        wait.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        wait.restype = ctypes.c_ulong
        try:
            assert all(wait(handle, 5000) == 0 for handle in handles)
        finally:
            for handle in handles:
                api._windows().close(handle)
    else:
        for pid in pids:
            assert not psutil.pid_exists(pid) or psutil.Process(pid).status() == psutil.STATUS_ZOMBIE


def test_cancelled_command_never_spawns(tmp_path):
    AnalysisError = platform().AnalysisError
    api = platform()
    cancelled = threading.Event()
    cancelled.set()
    marker = tmp_path / "must-not-exist"
    with pytest.raises(AnalysisError, match="cancelled"):
        api.native_run([sys.executable, "-c", f"open({str(marker)!r},'w').close()"], tmp_path, 2, None, cancelled)
    assert not marker.exists()


@pytest.mark.skipif(sys.platform != "win32", reason="Windows job and batch contracts")
def test_successful_launcher_can_leave_managed_session_alive(tmp_path):
    import json
    api = platform()
    marker = tmp_path / "session.json"
    source = ("import subprocess,sys,json; "
              "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(60)'],"
              "stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL); "
              f"open({str(marker)!r},'w').write(json.dumps(p.pid))")
    result = api.native_run([sys.executable, "-c", source], tmp_path, 5, None, threading.Event())
    assert result.returncode == 0
    pid = json.loads(marker.read_text())
    native = api._windows()
    handle = native.open_process(pid, terminate=True)
    try:
        import ctypes
        wait = ctypes.WinDLL("kernel32").WaitForSingleObject
        wait.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        assert wait(handle, 0) == 258, "successful launcher destroyed managed session"
    finally:
        native.terminate(handle)
        native.close(handle)


@pytest.mark.skipif(sys.platform != "win32", reason="Windows shims")
def test_cmd_shim_resolution_and_unsafe_input_rejection(tmp_path, monkeypatch):
    AnalysisError = platform().AnalysisError
    api = platform()
    directory = tmp_path / "shim space ü"
    directory.mkdir()
    (directory / "quality-fixture.cmd").write_text('@echo off\r\necho %~1\r\n', encoding="utf-8")
    env = dict(os.environ, PATH=str(directory) + os.pathsep + os.environ["PATH"])
    result = api.native_run(["quality-fixture", "two spaces"], tmp_path, 5, env, threading.Event())
    assert result.returncode == 0 and result.stdout.strip() == "two spaces"
    sentinel = tmp_path / "injected-sentinel"
    for value in [f'& echo injected > {sentinel}', "%PATH%", "!PATH!", 'bad"quote', "a\nb"]:
        with pytest.raises(AnalysisError, match="Unsafe Windows batch"):
            api.native_run(["quality-fixture", value], tmp_path, 5, env, threading.Event())
        assert not sentinel.exists()


@pytest.mark.skipif(sys.platform != "win32", reason="Windows suspended assignment")
def test_job_assignment_failure_never_executes_unowned_code(tmp_path, monkeypatch):
    AnalysisError = platform().AnalysisError
    api = platform()
    native = api._windows()
    def denied(*_):
        raise PermissionError("job assignment denied")
    monkeypatch.setattr(native, "attach_and_resume", denied)
    marker = tmp_path / "escaped"
    with pytest.raises(AnalysisError, match="native process permissions"):
        api.native_run([sys.executable, "-c", f"open({str(marker)!r},'w').close()"], tmp_path, 5, None, threading.Event())
    assert not marker.exists()


def test_quality_support_delegates_shared_cancellation(monkeypatch, tmp_path):
    api = platform()
    import analyze as support
    calls = []
    monkeypatch.setattr(api, 'native_run', lambda *args: calls.append(args) or "completed")
    assert support.quality_run(["tool"], tmp_path, 3, {"PATH": "test"}) == "completed"
    assert calls == [(["tool"], tmp_path, 3, {"PATH": "test"}, support.CANCELLED)]


def test_support_lock_uses_native_facade_and_always_unlocks(monkeypatch, tmp_path):
    api = platform()
    import analyze as support
    calls = []
    monkeypatch.setattr(api, 'try_lock', lambda stream: calls.append("try") or True)
    monkeypatch.setattr(api, 'unlock', lambda stream: calls.append("unlock"))
    with pytest.raises(RuntimeError):
        with support.lock(tmp_path / "lease"):
            raise RuntimeError("body")
    assert calls == ["try", "unlock"]


def test_support_cache_uses_native_root(monkeypatch, tmp_path):
    api = platform()
    import analyze as support
    monkeypatch.setattr(api, 'cache_home', lambda: tmp_path / "native-cache")
    monkeypatch.setattr(support, 'run_git', lambda *args: b".git")
    root = tmp_path / "repository"
    assert support.cache_directory(root).parent == tmp_path / "native-cache"


def test_native_process_table_exposes_only_quality_markers():
    api = platform()
    process = api.native_processes()[os.getpid()]
    assert process["parent"] == os.getppid()
    assert process["started"].isdigit()
    assert process["argv"]
    expected_executable = sys._base_executable if sys.platform == "win32" else sys.executable
    assert Path(process["executable"]).resolve() == Path(expected_executable).resolve()
    assert set(process["markers"]) <= {"HORO_QUALITY_SESSION", "HORO_QUALITY_PROFILE"}


@pytest.mark.parametrize("host", ["127.0.0.1", "::1", "0.0.0.0"])
def test_native_listener_accepts_only_owned_loopback_listeners(host):
    import socket
    api = platform()
    family = socket.AF_INET6 if ":" in host else socket.AF_INET
    with socket.socket(family) as listener:
        listener.bind((host, 0))
        listener.listen()
        port = listener.getsockname()[1]
        expected = None if host == "0.0.0.0" else (os.getpid(), port)
        assert api.native_listener_port({os.getpid()}, range(port, port + 1)) == expected
        assert api.native_listener_port(set(), range(port, port + 1)) is None


def test_native_listener_rejects_ambiguity():
    import socket
    AnalysisError = platform().AnalysisError
    api = platform()
    with socket.socket() as first, socket.socket() as second:
        for listener in (first, second):
            listener.bind(("127.0.0.1", 0))
            listener.listen()
        ports = [listener.getsockname()[1] for listener in (first, second)]
        with pytest.raises(AnalysisError, match="ambiguous"):
            api.native_listener_port({os.getpid()}, range(min(ports), max(ports) + 1))


@pytest.mark.skipif(sys.platform != "win32", reason="Pinned Windows identity contract")
@pytest.mark.parametrize("failure", ["reuse", "permission", "gone", "success"])
def test_windows_termination_pins_and_validates_before_any_signal(monkeypatch, failure):
    AnalysisError = platform().AnalysisError
    api = platform()
    events = []
    class Native:
        def open_process(self, pid, terminate=False):
            events.append(("open", pid, terminate))
            if pid == 12 and failure == "permission":
                raise PermissionError("denied")
            if pid == 12 and failure == "gone":
                raise ProcessLookupError(pid)
            return pid + 100
        def started(self, handle):
            return "reused" if handle == 112 and failure == "reuse" else "77"
        def terminate(self, handle):
            events.append(("terminate", handle))
        def close(self, handle):
            events.append(("close", handle))
    monkeypatch.setattr(api, '_windows', lambda: Native())
    table = {11: {"started": "77"}, 12: {"started": "77"}}
    if failure == "permission":
        with pytest.raises(AnalysisError, match="permissions"):
            api.terminate_processes({11, 12}, table)
    else:
        assert api.terminate_processes({11, 12}, table) is (failure != "reuse")
    if failure in {"reuse", "permission"}:
        assert not any(event[0] == "terminate" for event in events)
    else:
        assert ("terminate", 111) in events
    assert ("close", 111) in events
    if failure in {"reuse", "success"}:
        assert ("close", 112) in events


def test_termination_requires_complete_supplied_creation_identity(monkeypatch):
    api = platform()
    monkeypatch.setattr(api, '_windows', lambda: pytest.fail("must not open unverified PID"))
    assert not api.terminate_processes({123}, {})
    assert not api.terminate_processes({123}, {123: {"started": ""}})
    assert not api.terminate_processes({0}, {0: {"started": "77"}})


@pytest.mark.skipif(sys.platform != "win32", reason="Windows native error codes")
@pytest.mark.parametrize("code", [5, 6, 33])
def test_windows_lock_reports_only_lock_violation_as_contention(monkeypatch, tmp_path, code):
    import ctypes
    from types import SimpleNamespace
    api = platform()
    native = api._windows()
    def fail(*args):
        ctypes.set_last_error(code)
        return 0
    monkeypatch.setattr(native, "kernel", SimpleNamespace(LockFileEx=fail), raising=False)
    with (tmp_path / "lease").open("a") as stream:
        if code == 33:
            assert api.try_lock(stream) is False
        else:
            with pytest.raises(OSError) as raised:
                api.try_lock(stream)
            assert raised.value.winerror == code


def test_native_lock_excludes_another_process(tmp_path):
    api = platform()
    path = tmp_path / "cross-process-lease"
    source = (f"import sys; sys.path.insert(0,{str(Path(__file__).resolve().parents[2] / 'scripts')!r}); "
              f"import analyze as p; f=open({str(path)!r},'a'); print(p.try_lock(f))")
    with path.open("a") as stream:
        assert api.try_lock(stream)
        try:
            assert api.native_run([sys.executable, "-c", source], tmp_path, 5, None, threading.Event()).stdout.strip() == "False"
        finally:
            api.unlock(stream)
    assert api.native_run([sys.executable, "-c", source], tmp_path, 5, None, threading.Event()).stdout.strip() == "True"


def test_native_verified_termination_rejects_stale_identity_then_stops_owned_child(tmp_path):
    import subprocess
    import time
    api = platform()
    process = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + 5
        while True:
            table = api.native_processes()
            if process.pid in table:
                break
            assert time.monotonic() < deadline
            time.sleep(0.01)
        original = table[process.pid]
        stale = {process.pid: dict(original, started="stale")}
        assert not api.terminate_processes({process.pid}, stale)
        assert process.poll() is None
        assert api.terminate_processes({process.pid}, table)
        assert process.wait(timeout=5) != 0
    finally:
        if process.poll() is None:
            process.kill()  # Popen's pinned handle: this fixture's own child only.
        process.wait(timeout=5)


@pytest.mark.parametrize("failure", ["reuse", "permission", "success"])
def test_linux_pidfds_are_all_validated_before_signals(monkeypatch, failure):
    api = platform()
    AnalysisError = platform().AnalysisError
    events = []
    monkeypatch.setattr(api.sys, 'platform', "linux")
    def open_pid(pid):
        if failure == "permission" and pid == 12:
            raise PermissionError("denied")
        events.append(("open", pid))
        return pid + 100
    monkeypatch.setattr(api.os, 'pidfd_open', open_pid, raising=False)
    monkeypatch.setattr(api.signal, 'SIGKILL', 9, raising=False)
    monkeypatch.setattr(api.signal, 'pidfd_send_signal', lambda fd, sig: events.append(("signal", fd, sig)), raising=False)
    monkeypatch.setattr(api.os, 'close', lambda fd: events.append(("close", fd)))
    monkeypatch.setattr(api.time, 'sleep', lambda _: None)
    current = {11: {"started": "77"}, 12: {"started": "88" if failure == "reuse" else "77"}}
    monkeypatch.setattr(api, 'native_processes', lambda: current)
    if failure == "permission":
        with pytest.raises(AnalysisError, match="permissions"):
            api.terminate_processes({11, 12}, {11: {"started": "77"}, 12: {"started": "77"}})
    else:
        assert api.terminate_processes({11, 12}, {11: {"started": "77"}, 12: {"started": "77"}}) is (failure == "success")
    assert ("close", 111) in events
    if failure != "success":
        assert not any(event[0] == "signal" for event in events)
    else:
        assert len([event for event in events if event[0] == "signal"]) == 4


@pytest.mark.skipif(sys.platform != "win32", reason="Windows cleanup failure contract")
def test_job_cleanup_failure_is_typed_and_always_closes_job(tmp_path, monkeypatch):
    AnalysisError = platform().AnalysisError
    api = platform()
    native = api._windows()
    events = []
    create_job = native.create_job
    close = native.close
    def created():
        job = create_job()
        events.append(("job", job))
        return job
    def closed(handle):
        events.append(("close", handle))
        close(handle)
    def denied(job):
        raise PermissionError("job cleanup denied")
    monkeypatch.setattr(native, "create_job", created)
    monkeypatch.setattr(native, "close", closed)
    monkeypatch.setattr(native, "kill_job", denied)
    with pytest.raises(AnalysisError, match="cleanup"):
        api.native_run([sys.executable, "-c", "import time; time.sleep(60)"], tmp_path, 0.05, None, threading.Event())
    assert ("close", events[0][1]) in events


def test_unknown_platform_fails_closed(monkeypatch):
    api = platform()
    monkeypatch.setattr(api.sys, 'platform', "darwin")
    AnalysisError = platform().AnalysisError
    with pytest.raises(AnalysisError, match="Linux.*Windows"):
        api.ensure_supported()
