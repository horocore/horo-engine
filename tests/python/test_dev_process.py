"""Tiny subprocess fixtures exercise owned tree termination without engine builds."""

from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import sys
import time

import pytest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("horo_dev_process", ROOT / "scripts" / 'dev.py')
assert SPEC is not None
assert SPEC.loader is not None
process = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = process
SPEC.loader.exec_module(process)


def test_normal_exit_code_environment_and_argv_spaces(tmp_path: Path) -> None:
    output = tmp_path / "result with spaces türkçe.txt"
    program = "import os,pathlib,sys; pathlib.Path(sys.argv[1]).write_text(os.environ['PROOF']); sys.exit(7)"
    environment = dict(os.environ, PROOF="value with spaces")
    assert process.run([sys.executable, "-c", program, str(output)], tmp_path, environment) == 7
    assert output.read_text() == "value with spaces"
    assert process.run([sys.executable, "-c", "pass"], tmp_path) == 0


def test_missing_command_preserves_launch_error(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError):
        process.run([str(tmp_path / "missing command")], tmp_path)


@pytest.mark.skipif(os.name == "nt", reason="POSIX birth-verified descendant cleanup fixture")
def test_interruption_waits_for_root_and_stubborn_descendant(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    pid_file = tmp_path / "owned-pids.txt"
    script = tmp_path / "parent with spaces.py"
    child = "import signal,time; signal.signal(signal.SIGINT,signal.SIG_IGN); signal.signal(signal.SIGTERM,signal.SIG_IGN); time.sleep(30)"
    script.write_text(
        "import os,pathlib,subprocess,sys,time\n"
        f"child=subprocess.Popen([sys.executable,'-c',{child!r}])\n"
        "time.sleep(.15)\n"
        f"pathlib.Path({str(pid_file)!r}).write_text(str(os.getpid())+' '+str(child.pid))\n"
        "time.sleep(30)\n"
    )
    snapshot = process._snapshot
    interrupted = False

    def interrupt_after_spawn():
        nonlocal interrupted
        if not interrupted:
            deadline = time.monotonic() + 5
            while not pid_file.exists() and time.monotonic() < deadline:
                time.sleep(.02)
            assert pid_file.exists(), "Fixture parent did not spawn"
            interrupted = True
            raise KeyboardInterrupt
        return snapshot()

    monkeypatch.setattr(process, '_snapshot', interrupt_after_spawn)
    assert process.run([sys.executable, str(script)], tmp_path) == 130
    table = snapshot()
    root, child_pid = map(int, pid_file.read_text().split())
    assert root not in table  # Owned root was reaped, rather than merely signaled.
    assert child_pid not in table or table[child_pid].state.startswith(("Z", "X"))


def test_reused_pid_is_never_signaled(monkeypatch: pytest.MonkeyPatch) -> None:
    owned = process._Identity(123, 1, 123, "S", "original birth")
    reused = process._Identity(123, 1, 456, "S", "different birth")
    monkeypatch.setattr(process, '_snapshot', lambda: {123: reused})
    monkeypatch.setattr(process.os, 'kill', lambda *_: pytest.fail("Reused PID must not be signaled"))
    process._signal(owned, process.signal.SIGTERM)


@pytest.mark.skipif(os.name == "nt", reason="POSIX cleanup failure fixture")
def test_cleanup_failure_never_returns_clean_cancellation(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path,
) -> None:

    class Child:
        pid = 123

        def wait(self, **kwargs):
            raise KeyboardInterrupt

    monkeypatch.setattr(process, 'validated_command', lambda command, *_: list(command))
    monkeypatch.setattr(process.subprocess, 'Popen', lambda *_, **__: Child())
    monkeypatch.setattr(process, '_snapshot', lambda: {})

    def failed(*_):
        raise OSError("descendant remains active")
    monkeypatch.setattr(process, '_cleanup', failed)
    with pytest.raises(process.CleanupError, match="cleanup was not confirmed"):
        process.run(["fixture"], tmp_path)


@pytest.mark.skipif(os.name == "nt", reason="POSIX cleanup cancellation fixture")
@pytest.mark.parametrize("cancellation", [KeyboardInterrupt(), SystemExit(23)])
@pytest.mark.parametrize("failure", [KeyboardInterrupt(), OSError("inventory failed")])
def test_cancellation_during_cleanup_preserves_exception_and_unconfirmed_cause(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, cancellation: BaseException, failure: BaseException,
) -> None:
    class Child:
        pid = 123

    monkeypatch.setattr(process, 'validated_command', lambda command, *_: list(command))
    monkeypatch.setattr(process.subprocess, 'Popen', lambda *_, **__: Child())

    def fail_inventory():
        raise failure

    def cancel_cleanup(*_):
        raise cancellation

    monkeypatch.setattr(process, '_snapshot', fail_inventory)
    monkeypatch.setattr(process, '_cleanup', cancel_cleanup)
    with pytest.raises(type(cancellation)) as caught:
        process.run(["fixture"], tmp_path)
    assert caught.value is cancellation
    assert isinstance(caught.value.__cause__, process.CleanupError)


@pytest.mark.parametrize("cancellation", [KeyboardInterrupt(), SystemExit(23)])
def test_windows_cleanup_cancellation_preserves_exception_and_unconfirmed_cause(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, cancellation: BaseException,
) -> None:
    class Child:
        def __enter__(self):
            return self

        def __exit__(self, *_):
            return False

        def wait(self):
            raise KeyboardInterrupt

        def terminate(self):
            raise cancellation

    monkeypatch.setattr(process.subprocess, 'CREATE_NEW_PROCESS_GROUP', 0, raising=False)
    monkeypatch.setattr(process.subprocess, 'Popen', lambda *_, **__: Child())
    with pytest.raises(type(cancellation)) as caught:
        process._windows_run(["fixture"], tmp_path, None)
    assert caught.value is cancellation
    assert isinstance(caught.value.__cause__, process.CleanupError)


def test_discovery_excludes_unrelated_and_reused_parent_descendants() -> None:
    owned = process._Identity(100, 1, 100, "S", "owned root")
    child = process._Identity(200, 100, 100, "S", "owned child")
    unrelated = process._Identity(300, 1, 300, "S", "unrelated")
    known = {100: owned}
    process._discover(100, known, {100: owned, 200: child, 300: unrelated})
    assert set(known) == {100, 200}
    reused = process._Identity(100, 1, 100, "S", "reused root")
    new_child = process._Identity(400, 100, 100, "S", "unrelated child")
    process._discover(100, known, {100: reused, 400: new_child})
    assert 400 not in known


@pytest.mark.skipif(os.name == "nt", reason="POSIX signal permission fixture")
def test_permission_failure_still_signals_other_owned_children(monkeypatch: pytest.MonkeyPatch) -> None:
    denied = process._Identity(100, 1, 100, "S", "denied root")
    child = process._Identity(200, 100, 100, "S", "owned child")
    known = {100: denied, 200: child}
    calls = []

    def send(record, number):
        calls.append(record.pid)
        if record.pid == 200:
            raise PermissionError("denied child signal")
    # Terminal follow-up verifies that a transient signal denial need not cause false failure.
    inventories = iter([{100: denied, 200: child}, {}])
    monkeypatch.setattr(process, '_snapshot', lambda: next(inventories))
    monkeypatch.setattr(process, '_signal', send)

    class Root:
        pid = 100

        def poll(self):
            return 0

        def wait(self, **kwargs):
            return 0
    process._cleanup(Root(), known)
    assert calls == [200, 100]


@pytest.mark.skipif(os.name == "nt", reason="POSIX ordinary inventory error fixture")
def test_inventory_error_stays_ordinary_after_confirmed_cleanup(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path,
) -> None:

    class Root:
        pid = 100
    monkeypatch.setattr(process, 'validated_command', lambda command, *_: list(command))
    monkeypatch.setattr(process.subprocess, 'Popen', lambda *_, **__: Root())

    def failed():
        raise OSError("inventory unavailable")
    monkeypatch.setattr(process, '_snapshot', failed)
    monkeypatch.setattr(process, '_cleanup', lambda *_: None)
    with pytest.raises(OSError, match="inventory unavailable") as error:
        process.run(["fixture"], tmp_path)
    assert not isinstance(error.value, process.CleanupError)


@pytest.mark.parametrize("command", ["python -c pass", [], [""], [sys.executable, 3], [sys.executable, "bad\0value"]])
def test_reject_invalid_argv_before_launch(command, tmp_path: Path) -> None:
    with pytest.raises(ValueError):
        process.validated_command(command, tmp_path)


def test_resolve_executable_from_relative_cwd(tmp_path: Path) -> None:
    binary = tmp_path / "local python"
    try:
        binary.symlink_to(Path(sys.executable).resolve())
    except OSError:
        pytest.skip("Executable symlinks require host permission")
    assert process.validated_command(["./local python", "-c", "pass"], tmp_path)[0] == str(binary)
    assert process.validated_command(["local python", "-c", "pass"], tmp_path, {"PATH": "."})[0] == str(binary)
