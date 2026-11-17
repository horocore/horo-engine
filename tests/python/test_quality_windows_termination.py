"""Real pinned Windows cleanup contracts across natural process-exit races."""
from __future__ import annotations

from contextlib import contextmanager
import ctypes
import importlib
import multiprocessing
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
pytestmark = pytest.mark.skipif(sys.platform != "win32", reason="Real pinned Windows process contracts")


def _wait_for_release(ready, release):
    ready.set()
    release.wait(timeout=30)


@contextmanager
def _pinned_waiting_child(native, *, terminate):
    """Pin only this fixture's process and release it through an owned Event."""
    context = multiprocessing.get_context("spawn")
    ready, release = context.Event(), context.Event()
    child = context.Process(target=_wait_for_release, args=(ready, release))
    child.start()
    handle = None
    try:
        assert ready.wait(timeout=5)
        handle = native.open_process(child.pid, terminate=terminate)
        assert native.started(handle) == native.started(child.sentinel)
        yield child, handle, release
    finally:
        release.set()
        child.join(timeout=5)
        try:
            assert child.exitcode == 0
        finally:
            if child.is_alive():
                child.kill()  # Only this fixture's created process, if the fixture itself fails.
                child.join(timeout=5)
            if handle is not None:
                native.close(handle)
            child.close()


def test_windows_termination_accepts_exit_after_live_handle_check(monkeypatch):
    native = importlib.import_module("analyze")._windows()
    with _pinned_waiting_child(native, terminate=True) as (child, handle, release):
        terminate = native.kernel.TerminateProcess
        raced = []

        def exit_before_terminate(requested, code):
            assert requested == handle
            release.set()
            child.join(timeout=5)
            assert child.exitcode == 0
            result = terminate(requested, code)
            assert not result and ctypes.get_last_error() == 5
            raced.append(True)
            return result

        monkeypatch.setattr(native.kernel, "TerminateProcess", exit_before_terminate)
        native.terminate(handle)
        assert raced == [True]
        assert child.exitcode == 0


def test_windows_termination_denied_for_live_pinned_process():
    native = importlib.import_module("analyze")._windows()
    with _pinned_waiting_child(native, terminate=False) as (child, handle, _release):
        with pytest.raises(PermissionError) as failure:
            native.terminate(handle)
        assert failure.value.winerror == 5
        assert child.is_alive()


def test_windows_termination_waits_for_concurrent_exit_after_access_denied(monkeypatch):
    native = importlib.import_module("analyze")._windows()
    with _pinned_waiting_child(native, terminate=False) as (child, handle, release):
        terminate = native.kernel.TerminateProcess
        wait = native.kernel.WaitForSingleObject
        denied = []

        def deny_before_exit(requested, code):
            assert requested == handle
            result = terminate(requested, code)
            assert not result and ctypes.get_last_error() == 5
            denied.append(True)
            return result

        def complete_exit_during_wait(requested, milliseconds):
            assert requested == handle
            if denied:
                if milliseconds == 0:
                    # Observe the real pending state before allowing the fixture to settle.
                    result = wait(requested, milliseconds)
                    assert result == 258
                    release.set()
                    return result
                release.set()
            return wait(requested, milliseconds)

        monkeypatch.setattr(native.kernel, "TerminateProcess", deny_before_exit)
        monkeypatch.setattr(native.kernel, "WaitForSingleObject", complete_exit_during_wait)
        native.terminate(handle)
        assert denied == [True]
        child.join(timeout=5)
        assert child.exitcode == 0
