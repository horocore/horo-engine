"""Build, setup, cache, process ownership, CI qualification and release tools."""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import nullcontext
from copy import deepcopy
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path, PurePosixPath
from typing import Any, Callable, Iterator, Mapping, Sequence
from urllib.parse import ParseResult, quote, urlencode, urlparse
import argparse
import asyncio
import calendar
import contextlib
import ctypes
import datetime
import errno
import hashlib
import http.client
import json
import os
import platform
import re
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tarfile
import textwrap
import time
import uuid
import zipfile


# Error

class HoroError(Exception):
    """An application error with the same owned payload exposed by other hosts."""

    def __init__(self, payload: dict[str, Any], exit_code: int) -> None:
        self.payload = deepcopy(payload)
        self.exit_code = exit_code
        self.domain = self.payload["domain"]
        self.code = self.payload["code"]
        self.severity = self.payload["severity"]
        self.message = self.payload["message"]
        self.diagnostics = self.payload["diagnostics"]
        self.metadata = self.payload["metadata"]
        self.cause = self.payload["cause"]
        super().__init__(self.message)


class HoroUsageError(HoroError):
    """CLI request syntax or option failure (exit 2)."""


class HoroValidationError(HoroError):
    """Application input validation failure (exit 3)."""


class HoroCapabilityError(HoroError):
    """Required capability unavailable (exit 4)."""


class HoroOperationError(HoroError):
    """Application operation failure (exit 5)."""


class HoroPermissionError(HoroError):
    """Security or permission refusal (exit 6)."""


class HoroCancelledError(HoroError):
    """Cooperative cancellation (exit 7)."""


class HoroTimeoutError(HoroError):
    """Declared deadline elapsed (exit 8)."""


class HoroInvariantError(HoroError):
    """Reported in-process invariant failure (exit 10)."""


_EXCEPTIONS: dict[int, type[HoroError]] = {
    2: HoroUsageError,
    3: HoroValidationError,
    4: HoroCapabilityError,
    5: HoroOperationError,
    6: HoroPermissionError,
    7: HoroCancelledError,
    8: HoroTimeoutError,
    10: HoroInvariantError,
}
_SEVERITIES = {"info", "warning", "error", "fatal"}


def _is_integer(value: Any) -> bool:
    """Accept integer schema values while rejecting JSON booleans."""
    return isinstance(value, int) and not isinstance(value, bool)


def _validate_payload(payload: Any, depth: int = 0) -> None:
    """Reject malformed or unbounded envelopes without inventing a business error."""
    if depth >= 16 or not isinstance(payload, dict):
        raise ValueError("Invalid Horo error payload")
    if set(payload) != {"domain", "code", "severity", "message", "diagnostics", "metadata", "cause"}:
        raise ValueError("Invalid Horo error fields")
    for key in ("domain", "code", "severity", "message"):
        value = payload[key]
        if not isinstance(value, str) or len(value.encode("utf-8")) > 4096:
            raise ValueError("Invalid Horo error text")
    if not payload["domain"] or not payload["code"] or payload["severity"] not in _SEVERITIES:
        raise ValueError("Invalid Horo error identity or severity")
    diagnostics = payload["diagnostics"]
    if not isinstance(diagnostics, list) or len(diagnostics) > 64 or payload["metadata"] != {}:
        raise ValueError("Invalid Horo error details")
    for diagnostic in diagnostics:
        _validate_diagnostic(diagnostic)
    if payload["cause"] is not None:
        _validate_payload(payload["cause"], depth + 1)


def _validate_diagnostic(diagnostic: Any) -> None:
    """Validate the canonical diagnostic vocabulary and location representation."""
    if not isinstance(diagnostic, dict) or set(diagnostic) != {"code", "severity", "message", "location", "path"}:
        raise ValueError("Invalid Horo diagnostic")
    for key in ("code", "severity", "message", "path"):
        value = diagnostic[key]
        if not isinstance(value, str) or len(value.encode("utf-8")) > 4096:
            raise ValueError("Invalid Horo diagnostic text")
    if not diagnostic["code"] or diagnostic["severity"] not in {"note", "warning", "error", "fatal"}:
        raise ValueError("Invalid Horo diagnostic identity or severity")
    location = diagnostic["location"]
    if not isinstance(location, dict) or set(location) != {"source", "line", "column"}:
        raise ValueError("Invalid Horo diagnostic location")
    if not isinstance(location["source"], str) or len(location["source"].encode("utf-8")) > 4096:
        raise ValueError("Invalid Horo diagnostic source")
    if any(not _is_integer(location[key]) or not 0 <= location[key] <= 0xFFFFFFFF for key in ("line", "column")):
        raise ValueError("Invalid Horo diagnostic coordinates")


def raise_application_error(envelope: dict[str, Any]) -> None:
    """Raise a typed exception only at the Python edge of a translated C++ failure.

    Malformed protocol/schema input raises ``ValueError``; it is never classified
    as a new application error. Host failure exit 1 and OS signals are excluded.
    """
    if not isinstance(envelope, dict) or set(envelope) != {"schemaVersion", "exitCode", "error"}:
        raise ValueError("Invalid Horo application-error envelope")
    if not _is_integer(envelope["schemaVersion"]) or envelope["schemaVersion"] != 1:
        raise ValueError("Unsupported Horo application-error schema")
    exit_code = envelope["exitCode"]
    if not _is_integer(exit_code) or exit_code not in _EXCEPTIONS:
        raise ValueError("Invalid Horo application-error exit category")
    _validate_payload(envelope["error"])
    raise _EXCEPTIONS[exit_code](envelope["error"], exit_code)


# Process

# Required stdlib for explicit, shell-free owned process execution.
  # nosec B404


class CleanupError(OSError):

    """Owned descendants were not proven terminal; callers must retain safety protection."""


@dataclass(frozen=True)
class _Identity:
    pid: int
    parent: int
    group: int
    state: str
    birth: str


def validated_command(command: Sequence[str], cwd: Path, env: Mapping[str, str] | None = None) -> list[str]:
    """Validate authorized argv and resolve its executable; this is not command authorization."""
    if isinstance(command, (str, bytes)) or not command or any(not isinstance(item, str) or "\0" in item for item in command):
        raise ValueError("A command must be a nonempty sequence of NUL-free strings")
    executable = command[0]
    if not executable:
        raise ValueError("A command executable must not be empty")
    resolved = _executable_path(executable, cwd.resolve(), env)
    if resolved is None:
        raise FileNotFoundError(errno.ENOENT, os.strerror(errno.ENOENT), executable)
    # Preserve the selected executable path: resolving a venv Python symlink changes its environment.
    return [os.path.abspath(resolved), *command[1:]]


def _executable_path(executable: str, directory: Path, env: Mapping[str, str] | None) -> str | None:
    """Resolve executable lookup from the child's working directory and PATH."""
    if Path(executable).is_absolute() or any(separator in executable for separator in ("/", "\\")):
        candidate = Path(executable)
        candidate = candidate if candidate.is_absolute() else directory / candidate
        resolved = shutil.which(str(candidate))
    else:
        paths = [str(Path(item) if Path(item).is_absolute() else directory / item)
                 for item in os.get_exec_path(env)]
        resolved = shutil.which(executable, path=os.pathsep.join(paths))
    return resolved


def _snapshot() -> dict[int, _Identity]:
    """Capture PID birth and ancestry together; command names never establish ownership."""
    # Fixed OS inventory binary and argv; process names cannot supply executable or options.
    result = subprocess.run(["/bin/ps", "-axo", "pid=,ppid=,pgid=,stat=,lstart="],  # nosec B603
                            capture_output=True, text=True, check=False, shell=False)
    if result.returncode:
        raise OSError(f"Cannot verify developer process ownership: {result.stderr.strip()}")
    records = {}
    for line in result.stdout.splitlines():
        fields = line.split(None, 4)
        if len(fields) != 5:
            raise OSError("Malformed process ownership inventory")
        pid, parent, group = map(int, fields[:3])
        records[pid] = _Identity(pid, parent, group, fields[3], fields[4].strip())
    return records


def _discover(root: int, known: dict[int, _Identity], table: dict[int, _Identity]) -> None:
    """A dedicated launch group discovers orphans; verified ancestry discovers other children."""
    original = known.get(root)
    current = table.get(root)
    group_owned = not (original and current and original.birth != current.birth)
    pending = list(table.values())
    while pending:
        additions = []
        for record in pending:
            parent = known.get(record.parent)
            live_parent = table.get(record.parent)
            verified_parent = parent and live_parent and parent.birth == live_parent.birth
            if (record.group == root and group_owned) or verified_parent:
                known[record.pid] = record
                additions.append(record)
        if not additions:
            return
        pending = [record for record in pending if record not in additions]


def _live(known: dict[int, _Identity], table: dict[int, _Identity]) -> list[_Identity]:
    return [record for pid, record in known.items() if pid in table
            and table[pid].birth == record.birth and not table[pid].state.startswith(("Z", "X"))]


def _depth(record: _Identity, known: dict[int, _Identity]) -> int:
    parents = set()
    while record.parent in known and record.parent not in parents:
        parents.add(record.parent)
        record = known[record.parent]
    return len(parents)


def _signal(record: _Identity, number: int) -> None:
    """Recheck exact PID/birth before signaling; never target a bare process group."""
    current = _snapshot().get(record.pid)
    if current is None or current.birth != record.birth or current.state.startswith(("Z", "X")):
        return
    try:
        if hasattr(os, "pidfd_open") and hasattr(signal, "pidfd_send_signal"):
            descriptor = os.pidfd_open(record.pid)
            try:
                # Revalidate after opening to bind the same process, even across PID reuse.
                latest = _snapshot().get(record.pid)
                if latest and latest.birth == record.birth:
                    signal.pidfd_send_signal(descriptor, number)
            finally:
                os.close(descriptor)
        else:
            os.kill(record.pid, number)
    except ProcessLookupError:
        pass


def _cleanup(process: subprocess.Popen, known: dict[int, _Identity]) -> None:
    """Stop children before their parent, then verify termination and reap the owned root."""
    last_error = None
    for number, grace in ((signal.SIGINT, 0.4), (signal.SIGTERM, 0.6), (signal.SIGKILL, 2.0)):
        table = _snapshot()
        _discover(process.pid, known, table)
        for record in sorted(_live(known, table), key=lambda item: _depth(item, known), reverse=True):
            try:
                _signal(record, number)
            except OSError as error:
                last_error = error
        deadline = time.monotonic() + grace
        while True:
            process.poll()
            table = _snapshot()
            _discover(process.pid, known, table)
            if not _live(known, table):
                process.wait(timeout=1)
                return
            if time.monotonic() >= deadline:
                break
            time.sleep(0.05)
    raise CleanupError("Developer subprocess cleanup could not verify all owned descendants terminated") from last_error


class _WindowsJob:
    """Contain descendants before user code runs; verify an empty kernel-owned job before release."""

    def __init__(self) -> None:
        from ctypes import wintypes as w

        class BasicLimits(ctypes.Structure):
            _fields_ = [("process_time", ctypes.c_int64), ("job_time", ctypes.c_int64),
                        ("flags", w.DWORD), ("min_ws", ctypes.c_size_t), ("max_ws", ctypes.c_size_t),
                        ("active", w.DWORD), ("affinity", ctypes.c_size_t), ("priority", w.DWORD), ("scheduling", w.DWORD)]

        class Limits(ctypes.Structure):
            _fields_ = [("basic", BasicLimits), ("io", ctypes.c_uint64 * 6),
                        ("process_memory", ctypes.c_size_t), ("job_memory", ctypes.c_size_t),
                        ("peak_process", ctypes.c_size_t), ("peak_job", ctypes.c_size_t)]

        class ThreadEntry(ctypes.Structure):
            _fields_ = [("size", w.DWORD), ("usage", w.DWORD), ("tid", w.DWORD), ("pid", w.DWORD),
                        ("base_priority", w.LONG), ("delta_priority", w.LONG), ("flags", w.DWORD)]

        class Accounting(ctypes.Structure):
            _fields_ = [("user", ctypes.c_int64), ("kernel", ctypes.c_int64),
                        ("period_user", ctypes.c_int64), ("period_kernel", ctypes.c_int64),
                        ("faults", w.DWORD), ("total", w.DWORD), ("active", w.DWORD), ("terminated", w.DWORD)]

        self._thread_entry, self._accounting = ThreadEntry, Accounting
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        signatures = {
            "CreateJobObjectW": ([ctypes.c_void_p, w.LPCWSTR], w.HANDLE),
            "SetInformationJobObject": ([w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD], w.BOOL),
            "AssignProcessToJobObject": ([w.HANDLE, w.HANDLE], w.BOOL),
            "TerminateJobObject": ([w.HANDLE, w.UINT], w.BOOL),
            "QueryInformationJobObject": ([w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD, ctypes.c_void_p], w.BOOL),
            "CloseHandle": ([w.HANDLE], w.BOOL),
            "CreateToolhelp32Snapshot": ([w.DWORD, w.DWORD], w.HANDLE),
            "Thread32First": ([w.HANDLE, ctypes.POINTER(ThreadEntry)], w.BOOL),
            "Thread32Next": ([w.HANDLE, ctypes.POINTER(ThreadEntry)], w.BOOL),
            "OpenThread": ([w.DWORD, w.BOOL, w.DWORD], w.HANDLE),
            "GetProcessIdOfThread": ([w.HANDLE], w.DWORD),
            "WaitForSingleObject": ([w.HANDLE, w.DWORD], w.DWORD),
            "ResumeThread": ([w.HANDLE], w.DWORD),
        }
        for name, (arguments, result) in signatures.items():
            function = getattr(self.kernel, name)
            function.argtypes, function.restype = arguments, result
        self.handle = self.check(self.kernel.CreateJobObjectW(None, None))
        try:
            limits = Limits()
            limits.basic.flags = 0x2000  # KILL_ON_JOB_CLOSE; no breakaway permission.
            self.check(self.kernel.SetInformationJobObject(self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)))
        except BaseException:
            self.close()
            raise

    @staticmethod
    def check(value):
        if not value:
            raise ctypes.WinError(ctypes.get_last_error())
        return value

    def attach_and_resume(self, process: subprocess.Popen) -> None:
        self.check(self.kernel.AssignProcessToJobObject(self.handle, int(process._handle)))
        # Popen closes the initial thread handle. Locate its sole suspended thread,
        # pin it, and check both its owner and the still-live original process handle.
        snapshot = self.kernel.CreateToolhelp32Snapshot(4, 0)
        if snapshot == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            entry = self._thread_entry()
            entry.size = ctypes.sizeof(entry)
            threads = []
            available = self.kernel.Thread32First(snapshot, ctypes.byref(entry))
            while available:
                if entry.pid == process.pid:
                    threads.append(entry.tid)
                available = self.kernel.Thread32Next(snapshot, ctypes.byref(entry))
            if ctypes.get_last_error() != 18:  # ERROR_NO_MORE_FILES
                raise ctypes.WinError(ctypes.get_last_error())
            if len(threads) != 1:
                raise OSError("Cannot identify sole suspended developer process thread")
            thread = self.check(self.kernel.OpenThread(0x0802, False, threads[0]))  # QUERY_LIMITED_INFORMATION | SUSPEND_RESUME
            try:
                if (self.check(self.kernel.GetProcessIdOfThread(thread)) != process.pid
                        or self.kernel.WaitForSingleObject(int(process._handle), 0) != 258):
                    raise OSError("Suspended developer process identity changed")
                if self.kernel.ResumeThread(thread) != 1:
                    raise OSError("Unexpected developer process thread suspend count")
            finally:
                self.check(self.kernel.CloseHandle(thread))
        finally:
            self.check(self.kernel.CloseHandle(snapshot))

    def stop(self) -> None:
        self.check(self.kernel.TerminateJobObject(self.handle, 130))
        deadline = time.monotonic() + 5
        while True:
            accounting = self._accounting()
            self.check(self.kernel.QueryInformationJobObject(
                self.handle, 1, ctypes.byref(accounting), ctypes.sizeof(accounting), None))
            if accounting.active == 0:
                return
            if time.monotonic() >= deadline:
                raise OSError("Timed out waiting for owned Windows developer process tree")
            time.sleep(0.01)

    def close(self) -> None:
        self.check(self.kernel.CloseHandle(self.handle))


def _windows_run(command: Sequence[str], cwd: Path, env: Mapping[str, str] | None) -> int:
    """Start suspended, contain the complete tree, and return 130 only after verified cancellation."""
    job = _WindowsJob()
    process = None
    try:
        # CREATE_SUSPENDED prevents child creation before containment, including during attach failures.
        process = subprocess.Popen(command, cwd=cwd, env=env, shell=False,
                                   creationflags=subprocess.CREATE_NEW_PROCESS_GROUP | 0x4)
        job.attach_and_resume(process)
        try:
            return process.wait()
        except KeyboardInterrupt:
            return 130
    finally:
        try:
            job.stop()
            if process is not None:
                if process.poll() is None:  # Attach may have failed with the root still suspended outside the job.
                    process.terminate()
                process.wait(timeout=5)
        except (KeyboardInterrupt, SystemExit) as cleanup_error:
            raise cleanup_error from CleanupError("Windows developer process cleanup was not confirmed")
        except Exception as cleanup_error:
            raise CleanupError("Windows developer process cleanup was not confirmed") from cleanup_error
        finally:
            try:
                if process is not None:
                    process._handle.Close()
            finally:
                job.close()


def run(command: Sequence[str], cwd: Path, env: Mapping[str, str] | None = None) -> int:
    """Run argv without a shell; return 130 only after verified owned-tree cleanup."""
    command = validated_command(command, cwd, env)
    if os.name == "nt":
        return _windows_run(command, cwd, env)
    # Caller-authorized absolute executable and validated argv; a shell never interprets arguments.
    # S8705: this local developer runner accepts operator-selected tool arguments, not agent-tool input.
    # Sonar properties use single -DKEY=VALUE entries; extra scanner flags are explicit CLI options.
    # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit
    process = subprocess.Popen(command, cwd=cwd, env=env, start_new_session=True, shell=False)  # NOSONAR  # nosec B603
    known: dict[int, _Identity] = {}
    try:
        while True:
            table = _snapshot()
            _discover(process.pid, known, table)
            try:
                code = process.wait(timeout=0.5)
                _cleanup(process, known)
                return code
            except subprocess.TimeoutExpired:
                continue
    except KeyboardInterrupt:
        try:
            _cleanup(process, known)
        except (KeyboardInterrupt, SystemExit) as error:
            raise error from CleanupError("Interrupted developer subprocess cleanup was not confirmed")
        except Exception as error:
            raise CleanupError("Interrupted developer subprocess cleanup was not confirmed") from error
        return 130
    except CleanupError:
        raise
    except BaseException:
        # Inventory/launch errors also must not silently release an active owned root.
        try:
            _cleanup(process, known)
        except (KeyboardInterrupt, SystemExit) as error:
            raise error from CleanupError("Developer subprocess cleanup after failure was not confirmed")
        except Exception as error:
            raise CleanupError("Developer subprocess cleanup after failure was not confirmed") from error
        raise


# Cache

# Git metadata/worktree operations use explicit argv without a shell.
  # nosec B404


_GENERATION = re.compile(r"gen-[0-9a-f]{32}")
_COMMIT = re.compile(r"[0-9a-f]{40}(?:[0-9a-f]{24})?")
_OWNER = "horo-local-build-cache-v1"
_CLEANUP_MARKER = "cleanup-unverified.json"
_READONLY_CONFIG = "readonly.conf"
_GENERATION_MARKER = "generation.json"
_MAX_SIZE = re.compile(r"[1-9]\d*(?:[KMGT]i?B?|[kmgt])?", re.ASCII)


class CacheError(RuntimeError):

    """A local cache operation could not complete safely."""


@dataclass(frozen=True)
class CacheBinding:

    """Compiler launcher and private environment for one leased generation."""

    launcher: str
    env: dict[str, str]
    generation: str
    main_commit: str


def _git(repo: Path, *arguments: str) -> str:
    executable = shutil.which("git")
    if executable is None:
        raise CacheError("Git is required for local build cache ownership")
    command = [os.path.abspath(executable), "-c", "core.fsmonitor=false", "-C", str(repo), *arguments]
    # The local operator owns PATH and the checkout; executable discovery preserves that tool choice.
    # Private callers supply fixed Git subcommands and pinned refs; paths remain separate argv entries.
    # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit, python.lang.security.audit.dangerous-subprocess-use-tainted-env-args.dangerous-subprocess-use-tainted-env-args
    result = subprocess.run(command, check=False, capture_output=True, text=True, shell=False)  # nosec B603
    if result.returncode:
        raise CacheError(f"git {' '.join(arguments[:2])} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def cache_directory(repo: Path) -> Path:
    """Share checkpoints across linked worktrees, separately for each host platform."""
    common = Path(_git(repo, "rev-parse", "--git-common-dir"))
    common = (common if common.is_absolute() else repo / common).resolve()
    identity = hashlib.sha256(os.fsencode(common)).hexdigest()[:24]
    if sys.platform == "win32":
        base = Path(os.environ.get("LOCALAPPDATA", str(Path.home() / "AppData" / "Local")))
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Caches"
    else:
        base = Path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache")))
    return base / "horo-build" / identity / f"{sys.platform}-{platform.machine()}"


def _safe_root(root: Path, repo: Path) -> None:
    """Never manage storage through symlinks or inside any source checkout."""
    if any(path.is_symlink() for path in (root, *root.parents)):
        raise CacheError("Local build cache path must not contain symlinks")
    resolved = root.resolve()
    sources = [Path(item[len("worktree "):]).resolve()
               for item in _git(repo, "worktree", "list", "--porcelain", "-z").split("\0")
               if item.startswith("worktree ")]
    common = Path(_git(repo, "rev-parse", "--git-common-dir"))
    common = (common if common.is_absolute() else repo / common).resolve()
    if any(resolved.is_relative_to(source) for source in sources) or resolved.is_relative_to(common):
        raise CacheError("Local build cache must be outside source and Git directories")
    if root.exists() and not root.is_dir():
        raise CacheError("Local build cache root is not a directory")


def _atomic_json(path: Path, value: dict[str, object]) -> None:
    temporary = path.with_name(f".{path.name}-{uuid.uuid4().hex}.tmp")
    try:
        with temporary.open("x", encoding="utf-8") as stream:
            json.dump(value, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def _read_json(path: Path) -> dict[str, object] | None:
    if path.is_symlink():
        return None
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return value if isinstance(value, dict) else None


class _FileLock:

    """OS-owned byte/file locks release automatically on crash; Windows supports readers."""

    def __init__(self, path: Path, *, shared: bool = False, blocking: bool = True):
        self.path, self.shared, self.blocking = path, shared, blocking
        self.stream = None
        self.overlapped = None

    def __enter__(self) -> "_FileLock":
        if self.path.is_symlink():
            raise CacheError(f"Cache lock must not be a symlink: {self.path}")
        self.stream = self.path.open("a+b")
        try:
            if os.name == "nt":
                self._windows_lock()
            else:
                import fcntl
                flags = fcntl.LOCK_SH if self.shared else fcntl.LOCK_EX
                if not self.blocking:
                    flags |= fcntl.LOCK_NB
                fcntl.flock(self.stream.fileno(), flags)
        except BaseException:
            self.stream.close()
            self.stream = None
            raise
        return self

    def _windows_lock(self) -> None:
        import ctypes
        from ctypes import wintypes
        import msvcrt

        class Overlapped(ctypes.Structure):
            _fields_ = [("Internal", ctypes.c_size_t), ("InternalHigh", ctypes.c_size_t),
                        ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD),
                        ("hEvent", wintypes.HANDLE)]

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        lock = kernel.LockFileEx
        lock.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD,
                         wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(Overlapped)]
        lock.restype = wintypes.BOOL
        self.overlapped = Overlapped()
        flags = 0 if self.shared else 2
        # Poll nonblocking requests so Python can handle cancellation while waiting.
        while not lock(msvcrt.get_osfhandle(self.stream.fileno()), flags | 1, 0, 1, 0,
                       ctypes.byref(self.overlapped)):
            error = ctypes.get_last_error()
            if error not in (32, 33):
                raise ctypes.WinError(error)
            if not self.blocking:
                raise BlockingIOError(error, "Cache generation is in use")
            time.sleep(0.1)

    def __exit__(self, *_: object) -> None:
        if self.stream is None:
            return
        try:
            if os.name == "nt":
                import ctypes
                from ctypes import wintypes
                import msvcrt
                unlock = ctypes.WinDLL("kernel32", use_last_error=True).UnlockFileEx
                unlock.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD,
                                   wintypes.DWORD, ctypes.c_void_p]
                unlock.restype = wintypes.BOOL
                if not unlock(msvcrt.get_osfhandle(self.stream.fileno()), 0, 1, 0,
                              ctypes.byref(self.overlapped)):
                    raise ctypes.WinError(ctypes.get_last_error())
            else:
                import fcntl
                fcntl.flock(self.stream.fileno(), fcntl.LOCK_UN)
        finally:
            self.stream.close()
            self.stream = None


def _initialize(root: Path, repo: Path) -> None:
    _safe_root(root, repo)
    root.mkdir(parents=True, exist_ok=True)
    for name in ("generations", "leases"):
        directory = root / name
        if directory.is_symlink():
            raise CacheError(f"Cache directory must not be a symlink: {directory}")
        directory.mkdir(exist_ok=True)


@contextlib.contextmanager
def heavy_build_lock(repo: Path) -> Iterator[None]:
    """Serialize dev.py configure/compilation across all repository worktrees."""
    root = cache_directory(repo)
    _initialize(root, repo)
    with _FileLock(root / "heavy-build.lock"):
        if (root / _CLEANUP_MARKER).exists():
            raise CacheError(f"An earlier build has unverified descendants; inspect {root / _CLEANUP_MARKER} before further builds")
        yield


def record_cleanup_failure(repo: Path) -> None:
    """Preserve generations and block new builds until orphan ownership is audited."""
    root = cache_directory(repo)
    _initialize(root, repo)
    _atomic_json(root / _CLEANUP_MARKER, {"schema": 1, "reason": "subprocess descendant termination was not confirmed"})


def _ready(root: Path) -> tuple[Path, dict[str, object]] | None:
    pointer = _read_json(root / "ready.json")
    if not pointer or pointer.get("schema") != 1:
        return None
    name, commit = pointer.get("generation"), pointer.get("main_commit")
    if not isinstance(name, str) or not _GENERATION.fullmatch(name):
        return None
    if not isinstance(commit, str) or not _COMMIT.fullmatch(commit):
        return None
    directory = root / "generations" / name
    if any(path.is_symlink() for path in (root / "generations", directory, directory / "compiler",
                                         directory / _READONLY_CONFIG)):
        return None
    marker = _read_json(directory / _GENERATION_MARKER)
    if not marker or marker.get("schema") != 1 or marker.get("owner") != _OWNER or marker.get("state") != "ready":
        return None
    if marker.get("main_commit") != commit or marker.get("generation") != name:
        return None
    if not (directory / "compiler").is_dir() or not (directory / _READONLY_CONFIG).is_file():
        return None
    if not _valid_readonly_config(directory, marker):
        return None
    return directory, pointer


def _valid_readonly_config(directory: Path, marker: dict[str, object]) -> bool:
    """Admit only the exact manager-generated read-only compiler configuration."""
    size = marker.get("max_size")
    if not isinstance(size, str) or not _MAX_SIZE.fullmatch(size):
        return False
    try:
        return (directory / _READONLY_CONFIG).read_text(encoding="utf-8") == _config(size, readonly=True)
    except (OSError, UnicodeError):
        return False


def _binding(directory: Path, commit: str, source: Path, launcher: str,
             environ: Mapping[str, str] | None, *, readonly: bool) -> CacheBinding:
    environment = {key: value for key, value in (os.environ if environ is None else environ).items()
                   if not key.upper().startswith("CCACHE_")}
    environment.update({"CCACHE_DIR": str(directory / "compiler"),
                        "CCACHE_CONFIGPATH": str(directory / (_READONLY_CONFIG if readonly else "writer.conf")),
                        "CCACHE_BASEDIR": str(source.resolve())})
    return CacheBinding(launcher, environment, directory.name, commit)


@contextlib.contextmanager
def automatic_cache(repo: Path, env: Mapping[str, str] | None = None) -> Iterator[CacheBinding | None]:
    """Discover a completed checkpoint without creating storage on a miss."""
    root = cache_directory(repo)
    launcher = shutil.which("ccache")
    stack = contextlib.ExitStack()
    binding = None
    try:
        _safe_root(root, repo)
        candidate = _ready(root) if launcher else None
        if candidate and not (root / "leases").is_symlink() and (root / "leases").is_dir():
            directory, pointer = candidate
            stack.enter_context(_FileLock(root / "leases" / f"{directory.name}.lock", shared=True))
            # GC may have retired this generation between discovery and lock acquisition.
            current = _ready(root)
            if current is not None and current[0] == directory:
                binding = _binding(directory, str(pointer["main_commit"]), repo, launcher, env, readonly=True)
    except (CacheError, OSError):
        stack.close()
    with stack:
        yield binding


def status(repo: Path) -> dict[str, object]:
    """Report readiness without creating directories or changing cache metadata."""
    root = cache_directory(repo)
    try:
        _safe_root(root, repo)
        ready = _ready(root)
    except (CacheError, OSError):
        ready = None
    result: dict[str, object] = {"root": str(root), "ready": ready is not None,
                                 "launcher_available": shutil.which("ccache") is not None,
                                 "cleanup_blocked": (root / _CLEANUP_MARKER).exists()}
    if ready:
        result.update(ready[1])
    return result


def _config(max_size: str, *, readonly: bool) -> str:
    # ccache accepts multiline remote_storage values; an empty value must be last.
    return (f"compiler_check = content\nsloppiness =\nmax_size = {max_size}\n"
            f"read_only = {'true' if readonly else 'false'}\nremote_storage =\n")


def _write_configs(directory: Path, max_size: str) -> None:
    for name, readonly in (("writer.conf", False), (_READONLY_CONFIG, True)):
        (directory / name).write_text(_config(max_size, readonly=readonly), encoding="utf-8")


def _seed(source: Path, destination: Path) -> None:
    """Copy compiler entries without aliases or links to the previous generation."""
    if any(path.is_symlink() for path in source.rglob("*")):
        raise CacheError("Previous compiler generation contains symlinks; refusing to copy")
    shutil.copytree(source, destination, dirs_exist_ok=True, copy_function=shutil.copy2)


def _retirement_candidate(directory: Path) -> bool:
    """Keep unknown generations and partial producer worktrees recoverable."""
    marker = _read_json(directory / _GENERATION_MARKER)
    if not marker or marker.get("schema") != 1 or marker.get("owner") != _OWNER or marker.get("state") != "ready":
        return False
    if marker.get("generation") != directory.name:
        return False
    return not (directory / "source").exists() and not (directory / "source").is_symlink()


def _retire(root: Path, active: str) -> None:
    """Delete only ready manager-owned generations whose readers have all released."""
    if any(path.is_symlink() for path in (root, *root.parents, root / "generations", root / "leases")):
        raise CacheError("Refusing retirement through a symlinked cache directory")
    if (root / _CLEANUP_MARKER).exists():
        return
    for directory in (root / "generations").iterdir():
        if directory.name == active or not _GENERATION.fullmatch(directory.name) or directory.is_symlink():
            continue
        if not _retirement_candidate(directory):
            continue
        try:
            with _FileLock(root / "leases" / f"{directory.name}.lock", blocking=False):
                if (root / _CLEANUP_MARKER).exists():
                    return
                if directory.resolve().parent != (root / "generations").resolve():
                    raise CacheError("Refusing generation retirement outside managed storage")
                shutil.rmtree(directory)
        except OSError:
            continue


def _new_candidate(root: Path, commit: str, max_size: str) -> tuple[Path, dict[str, object]]:
    """Create an unpublished, manager-owned generation before seeding or checkout creation."""
    name = f"gen-{uuid.uuid4().hex}"
    directory = root / "generations" / name
    directory.mkdir()
    (root / "leases" / f"{name}.lock").touch()
    marker: dict[str, object] = {"schema": 1, "owner": _OWNER, "generation": name,
                                 "main_commit": commit, "max_size": max_size, "state": "preparing"}
    _atomic_json(directory / _GENERATION_MARKER, marker)
    (directory / "compiler").mkdir()
    return directory, marker


def prepare_cache(repo: Path, build_callback: Callable[[Path, Path, CacheBinding], int],
                  max_size: str = "4G", main_ref: str = "origin/main",
                  env: Mapping[str, str] | None = None) -> int:
    """Build pinned clean main, then atomically publish; any failure preserves ready A."""
    if main_ref not in ("origin/main", "refs/remotes/origin/main"):
        raise CacheError("Only origin/main may produce local compiler checkpoints")
    if not _MAX_SIZE.fullmatch(max_size):
        raise CacheError("Cache max size must be a positive size such as 4G")
    launcher = shutil.which("ccache")
    if launcher is None:
        raise CacheError("ccache is required for cache prepare; install ccache and retry")
    root = cache_directory(repo)
    _initialize(root, repo)
    with _FileLock(root / "writer.lock"):
        if (root / _CLEANUP_MARKER).exists():
            raise CacheError("An earlier build has unverified descendants; checkpoint preparation is blocked")
        commit = _git(repo, "rev-parse", "--verify", f"{main_ref}^{{commit}}")
        if not _COMMIT.fullmatch(commit):
            raise CacheError("Cannot identify pinned origin/main commit")
        directory, marker = _new_candidate(root, commit, max_size)
        name = directory.name
        compiler = directory / "compiler"
        previous = _ready(root)
        if previous:
            with _FileLock(root / "leases" / f"{previous[0].name}.lock", shared=True):
                _seed(previous[0] / "compiler", compiler)
        _write_configs(directory, max_size)
        source = directory / "source"
        build = source / "build" / "dev-cache"
        _git(repo, "worktree", "add", "--detach", str(source), commit)
        if _git(source, "rev-parse", "HEAD") != commit or _git(source, "status", "--porcelain"):
            raise CacheError("Cache producer checkout must remain clean at pinned main")
        binding = _binding(directory, commit, source, launcher, env, readonly=False)
        code = build_callback(source, build, binding)
        if code:
            marker["state"] = "failed"
            _atomic_json(directory / _GENERATION_MARKER, marker)
            return code
        if _git(source, "rev-parse", "HEAD") != commit or _git(source, "status", "--porcelain"):
            marker["state"] = "failed"
            _atomic_json(directory / _GENERATION_MARKER, marker)
            raise CacheError("Cache producer modified pinned main; checkpoint was not published")
        _preserve_producer_recipe(build, directory)
        _git(repo, "worktree", "remove", str(source))
        marker["state"] = "ready"
        _atomic_json(directory / _GENERATION_MARKER, marker)
        _atomic_json(root / "ready.json", {"schema": 1, "generation": name, "main_commit": commit})
        _retire(root, name)
        return 0


def _preserve_producer_recipe(build: Path, generation: Path) -> None:
    """Retain diagnostic compiler recipes without retaining the disposable producer."""
    compile_commands = build / "compile_commands.json"
    if compile_commands.is_file():
        shutil.copy2(compile_commands, generation / "producer-compile-commands.json")


# Ci Cache

def dependency_fingerprint(root: Path) -> str:
    """Hash declarations and capability defaults, including file names and CRLF normalization."""
    paths = {
        root / "CMakeLists.txt",
        root / "cmake" / "Dependencies.cmake",
        *root.glob("cmake/*Dependency.cmake"),
        *root.glob("cmake/Horo*DependencyPolicy.cmake"),
        root / "cmake" / "HoroGnsProtobufConfig.cmake",
    }
    digest = hashlib.sha256(b"horo-ci-dependencies-v1\0")
    for path in sorted(paths):
        digest.update(path.relative_to(root).as_posix().encode("utf-8") + b"\0")
        digest.update(path.read_bytes().replace(bytes((13, 10)), b"\n") + b"\0")
    return digest.hexdigest()


def compiler_identity(compiler: str, text: str) -> str:
    """Validate version output from the composite action's fixed tool probes."""
    if compiler == "cl":
        match = re.search(r"Compiler Version (\d+(?:\.\d+)+)", text)
        family = "msvc"
    elif compiler in {"clang", "clang++"}:
        match = re.search(r"clang version (\d+(?:\.\d+)+)", text)
        family = "appleclang" if "Apple clang" in text else "clang"
    elif compiler in {"gcc", "g++"}:
        match = re.fullmatch(r"(\d+(?:\.\d+)+)", text.strip())
        family = "gcc"
    else:
        raise ValueError(f"Unsupported CI compiler {compiler}")
    if match is None:
        raise ValueError(f"Cannot identify compiler {compiler}")
    return f"{family}-{match.group(1)}"


def ci_cache_main(argv: list[str] | None = None) -> None:
    """Write validated toolchain and dependency identities to GitHub step outputs."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--github-output", type=Path, required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--cc-version", required=True)
    parser.add_argument("--cxx-version", required=True)
    parser.add_argument("--cmake-version", required=True)
    args = parser.parse_args(argv)
    root = Path(__file__).resolve().parents[1]
    match = re.search(r"cmake version (\d+\.\d+)\.", args.cmake_version)
    if match is None:
        raise ValueError("Cannot identify CMake version")
    identities = {
        "compiler": compiler_identity(args.cxx, args.cxx_version),
        "c-compiler": compiler_identity(args.cc, args.cc_version),
        "cmake": match.group(1),
        "dependencies": dependency_fingerprint(root),
    }
    with args.github_output.open("a", encoding="utf-8") as output:
        for key, value in identities.items():
            print(f"{key}={value}", file=output)
    print(f"Compiler: {identities['compiler']}; C compiler: {identities['c-compiler']}; CMake: {identities['cmake']}")


# Notes

#!/usr/bin/env python3


MAX_SOURCE_BYTES = 262144
MAX_SNAPSHOT_BYTES = 32768
MAX_ITEM_BYTES = 2048
MAX_ITEMS = 64
CATEGORIES = ("Added", "Changed", "Deprecated", "Removed", "Fixed", "Security")
HEADER = re.compile(r"^## \[([^\]]+)\](?: [—-] (\d{4}-\d{2}-\d{2}))?$", re.MULTILINE)


class NotesError(ValueError):
    """Actionable release-notes validation failure."""


def parse_version_core(core: str, text: str) -> tuple[int, int, int]:
    components = core.split(".")
    if len(components) != 3:
        raise NotesError(f"invalid semantic version '{text}'")
    for part in components:
        if not part.isascii() or not part.isdecimal() or (len(part) > 1 and part[0] == "0"):
            raise NotesError(f"invalid semantic version '{text}'")
    values = tuple(map(int, components))
    if any(value > 0xffffffff for value in values):
        raise NotesError(f"semantic version core overflows 32 bits: '{text}'")
    return values


def validate_identifiers(identifiers: str, numeric_leading_zero: bool, text: str) -> None:
    for part in identifiers.split("."):
        if not part or not part.isascii() or not all(char.isalnum() or char == "-" for char in part):
            raise NotesError(f"invalid semantic version '{text}'")
        if numeric_leading_zero and part.isdecimal() and len(part) > 1 and part[0] == "0":
            raise NotesError(f"noncanonical prerelease version '{text}'")


def version_parts(text: str) -> tuple:
    if not text or len(text) > 128:
        raise NotesError(f"invalid semantic version '{text[:128]}'")
    core_and_prerelease, separator, build = text.partition("+")
    core, prerelease_separator, prerelease = core_and_prerelease.partition("-")
    if (separator and not build) or (prerelease_separator and not prerelease):
        raise NotesError(f"invalid semantic version '{text}'")
    values = parse_version_core(core, text)
    if prerelease_separator:
        validate_identifiers(prerelease, True, text)
    if separator:
        validate_identifiers(build, False, text)
    return values + (prerelease, build)


def unsafe_markdown(item: str) -> bool:
    return (any(ord(char) < 32 or ord(char) == 127 for char in item)
            or "<" in item or ">" in item or re.search(r"!\[|\]\((?!https://)", item) is not None
            or item.count("`") % 2 != 0 or item.count("[") != item.count("]")
            or item.count("(") != item.count(")"))


def validate_item(item: str, version: str) -> str:
    if not item or len(item.encode("utf-8")) > MAX_ITEM_BYTES:
        raise NotesError(f"{version}: missing or oversized note item")
    if unsafe_markdown(item):
        raise NotesError(f"{version}: unsafe or malformed Markdown item")
    return item


def validate_date(version: str, date: str | None) -> None:
    try:
        if not date:
            raise ValueError()
        datetime.date.fromisoformat(date)
    except ValueError as error:
        raise NotesError(f"{version}: missing or invalid ISO release date") from error


def start_category(line: str, version: str, sections: list[dict]) -> dict:
    category = line[4:]
    if category not in CATEGORIES or any(part["category"] == category for part in sections):
        raise NotesError(f"{version}: unknown or repeated category '{category}'")
    section = {"category": category, "items": []}
    sections.append(section)
    return section


def parse_sections(version: str, lines: str) -> list[dict]:
    sections = []
    current = None
    count = 0
    for raw in lines.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("### "):
            current = start_category(line, version, sections)
        elif line.startswith("- ") and current is not None:
            count += 1
            if count > MAX_ITEMS:
                raise NotesError(f"{version}: too many note items")
            current["items"].append(validate_item(line[2:].strip(), version))
        else:
            raise NotesError(f"{version}: expected category heading or bullet near '{line[:64]}'")
    if not sections or any(not part["items"] for part in sections):
        raise NotesError(f"{version}: every category must have bullets")
    return sections


def parse_entry(version: str, date: str | None, lines: str) -> dict:
    version_parts(version)
    validate_date(version, date)
    return {"version": version, "date": date, "sections": parse_sections(version, lines)}


def release_headings(text: str) -> list[re.Match]:
    for line in text.splitlines():
        if line.startswith("## ") and HEADER.fullmatch(line) is None:
            raise NotesError(f"malformed release heading: {line[:80]}")
    headings = list(HEADER.finditer(text))
    if not headings:
        raise NotesError("CHANGELOG.md has no version headings")
    return headings


def parse_changelog(text: str) -> list[dict]:
    """Parse bounded released entries; Unreleased is never candidate content."""
    if len(text.encode("utf-8")) > MAX_SOURCE_BYTES:
        raise NotesError(f"CHANGELOG.md exceeds {MAX_SOURCE_BYTES} bytes")
    headings = release_headings(text)
    entries = []
    seen = set()
    for index, heading in enumerate(headings):
        version, date = heading.groups()
        if version == "Unreleased":
            if date:
                raise NotesError("[Unreleased] must not have a date")
            continue
        if version in seen:
            raise NotesError(f"duplicate release notes for {version}")
        seen.add(version)
        end = headings[index + 1].start() if index + 1 < len(headings) else len(text)
        entries.append(parse_entry(version, date, text[heading.end():end]))
    return entries


def select_version(entries: list[dict], version: str) -> dict:
    version_parts(version)
    for entry in entries:
        if entry["version"] == version:
            return entry
    raise NotesError(f"missing release notes for exact candidate version {version}")


def select_range(entries: list[dict], lower: str, upper: str) -> list[dict]:
    """Select released notes in (lower, upper], excluding Unreleased."""
    def precedence(version: str) -> tuple:
        major, minor, patch, prerelease, _build = version_parts(version)
        identifiers = tuple((0, int(part)) if part.isdigit() else (1, part)
                            for part in prerelease.split(".")) if prerelease else ()
        return major, minor, patch, bool(not prerelease), identifiers

    low, high = precedence(lower), precedence(upper)
    if low >= high:
        raise NotesError("release notes range must advance")
    return [entry for entry in entries if low < precedence(entry["version"]) <= high]


def render_markdown(entry: dict) -> str:
    lines = [f'## [{entry["version"]}] — {entry["date"]}']
    for section in entry["sections"]:
        lines += ["", f'### {section["category"]}']
        lines += [f"- {item}" for item in section["items"]]
    return "\n".join(lines) + "\n"


def snapshot_bytes(snapshot: dict) -> bytes:
    return (json.dumps(snapshot, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def make_snapshot(entry: dict, product: str, locale: str = "en-US") -> dict:
    if not re.fullmatch(r"[a-z][a-z0-9-]{1,63}", product):
        raise NotesError("product identity must be a bounded lowercase slug")
    if not re.fullmatch(r"[a-z]{2}-[A-Z]{2}", locale):
        raise NotesError("locale must be a language-region tag such as en-US")
    snapshot = {"schemaVersion": 1, "product": product, "version": entry["version"],
                "locale": locale, "date": entry["date"], "sections": entry["sections"],
                "markdown": render_markdown(entry)}
    if len(snapshot_bytes(snapshot)) > MAX_SNAPSHOT_BYTES:
        raise NotesError(f'{entry["version"]}: snapshot exceeds {MAX_SNAPSHOT_BYTES} bytes')
    return snapshot


def render_header(snapshot: dict) -> str:
    """Produce Welcome cards from exactly the frozen candidate snapshot."""
    items = [item for section in snapshot["sections"] for item in section["items"]][:2]
    items += [""] * (2 - len(items))
    title = f'v{snapshot["version"]} — {calendar.month_name[int(snapshot["date"][5:7])]} {snapshot["date"][:4]}'
    cards = [f'    {{"Release Notes", {json.dumps(title, ensure_ascii=False)}, '
             f'{json.dumps(item[:120], ensure_ascii=False)}}}' for item in items]
    return "\n".join((
        "// Generated from the reviewed release-notes snapshot. Do not edit.",
        "#pragma once", "", "namespace Horo::Generated {",
        "struct WhatsNewEntry { const char* tag; const char* title; const char* body; };",
        "inline constexpr int kWhatsNewCount = 2;",
        "inline constexpr WhatsNewEntry kWhatsNewEntries[2] = {",
        ",\n".join(cards), "};", "} // namespace Horo::Generated", ""
    ))


def generated_outputs() -> tuple[Path, Path]:
    """Keep generated output names fixed beneath the invocation's build directory."""
    directory = Path.cwd() / "generated"
    if directory.is_symlink():
        raise NotesError("generated output directory must not be a symlink")
    directory.mkdir(exist_ok=True)
    if not directory.is_dir():
        raise NotesError("generated output directory is not a directory")
    snapshot = directory / "ReleaseNotesSnapshot.json"
    header = directory / "GeneratedBuildInfo.h"
    if snapshot.is_symlink() or header.is_symlink():
        raise NotesError("generated output must not be a symlink")
    return snapshot, header


def notes_main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--product", required=True)
    args = parser.parse_args(argv)
    try:
        if not args.source.is_file() or args.source.stat().st_size > MAX_SOURCE_BYTES:
            raise NotesError("CHANGELOG.md is missing or oversized")
        entry = select_version(parse_changelog(args.source.read_text(encoding="utf-8")), args.version)
        snapshot = make_snapshot(entry, args.product)
        snapshot_path, header_path = generated_outputs()
        snapshot_path.write_bytes(snapshot_bytes(snapshot))
        header_path.write_text(render_header(snapshot), encoding="utf-8")
    except (OSError, UnicodeError, NotesError) as error:
        print(f"[release-notes] {error}", file=sys.stderr)
        return 1
    return 0


# Archive

#!/usr/bin/env python3


def notes_member(name: str) -> bool:
    """Accept only a root or single package-directory notes file."""
    if not name or "\\" in name or name.startswith("/"):
        raise NotesError(f"unsafe package member path: {name}")
    parts = PurePosixPath(name).parts
    if any(part in (".", "..") for part in name.split("/")) or any(not part for part in name.split("/")):
        raise NotesError(f"unsafe package member path: {name}")
    return parts[-1] == "release-notes.json" and len(parts) in (1, 2)


def verify_tar(snapshot: bytes, archive: Path) -> None:
    # Read-only tar stream: extractfile returns bytes and never materializes member paths.
    with tarfile.open(archive, "r:gz") as package:
        members = [member for member in package.getmembers()
                   if notes_member(member.name.rstrip("/"))]
        if len(members) != 1 or not members[0].isfile() or members[0].size != len(snapshot):
            raise NotesError("package must contain exactly one matching release-notes.json")
        content = package.extractfile(members[0])
        if content is None or content.read(MAX_SNAPSHOT_BYTES + 1) != snapshot:
            raise NotesError("packaged release notes differ from the reviewed snapshot")


def verify_zip(snapshot: bytes, archive: Path) -> None:
    with zipfile.ZipFile(archive) as package:
        members = [member for member in package.infolist()
                   if notes_member(member.filename.rstrip("/"))]
        if len(members) != 1 or members[0].is_dir() or members[0].file_size != len(snapshot):
            raise NotesError("package must contain exactly one matching release-notes.json")
        with package.open(members[0]) as content:
            if content.read(MAX_SNAPSHOT_BYTES + 1) != snapshot:
                raise NotesError("packaged release notes differ from the reviewed snapshot")


def verify_archive(snapshot: bytes, archive: Path) -> None:
    if not snapshot or len(snapshot) > MAX_SNAPSHOT_BYTES:
        raise NotesError("reviewed notes snapshot is missing or oversized")
    if archive.name.endswith(".tar.gz"):
        verify_tar(snapshot, archive)
    elif archive.suffix == ".zip":
        verify_zip(snapshot, archive)
    else:
        raise NotesError(f"unsupported release archive format: {archive.name}")


def archive_main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        verify_archive(args.snapshot.read_bytes(), args.archive)
    except (OSError, ValueError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(f"[release-notes] {error}", file=sys.stderr)
        return 1
    return 0


# Release

#!/usr/bin/env python3


def verify(snapshot_bytes: bytes, tag: str, cmake_text: str, release_body: str) -> None:
    if not tag.startswith("v"):
        raise NotesError("release tag must use v<exact-semver>")
    version = tag[1:]
    version_parts(version)
    declared = [line.removeprefix('set(HORO_ENGINE_VERSION "').removesuffix('")')
                for line in cmake_text.splitlines()
                if line.startswith('set(HORO_ENGINE_VERSION "') and line.endswith('")')]
    if declared != [version]:
        raise NotesError(f"release tag {tag} does not match HORO_ENGINE_VERSION")
    if not snapshot_bytes or len(snapshot_bytes) > MAX_SNAPSHOT_BYTES:
        raise NotesError("reviewed notes snapshot is missing or oversized")
    try:
        snapshot = json.loads(snapshot_bytes)
    except ValueError as error:
        raise NotesError("reviewed notes snapshot is malformed") from error
    if not isinstance(snapshot, dict):
        raise NotesError("reviewed notes snapshot must be an object")
    if (snapshot.get("schemaVersion") != 1 or snapshot.get("product") != "horo-editor"
        or snapshot.get("version") != version or snapshot.get("locale") != "en-US"):
        raise NotesError("reviewed notes snapshot product or exact version differs from release tag")
    if release_body.replace((chr(13) + chr(10)), "\n") != snapshot.get("markdown", ""):
        raise NotesError("GitHub Release body must equal the reviewed snapshot Markdown")


def release_body(tag: str) -> str:
    if not tag.startswith("v"):
        raise NotesError("release tag must use v<exact-semver>")
    version_parts(tag[1:])
    repository = os.environ.get("GITHUB_REPOSITORY", "")
    token = os.environ.get("GH_TOKEN", "")
    parts = repository.split("/")
    if (len(parts) != 2 or not all(part not in (".", "..") and all(char.isascii() and
        (char.isalnum() or char in "._-") for char in part) for part in parts) or not token):
        raise NotesError("GITHUB_REPOSITORY and GH_TOKEN are required for release verification")
    path = f"/repos/{repository}/releases/tags/{quote(tag, safe='')}"
    connection = http.client.HTTPSConnection("api.github.com", timeout=30)
    try:
        connection.request("GET", path, headers={"Accept": "application/vnd.github+json",
                                                 "Authorization": f"Bearer {token}",
                                                 "User-Agent": "horo-release-notes-verifier"})
        response = connection.getresponse()
        if response.status != 200:
            raise NotesError(f"GitHub Release lookup failed with HTTP {response.status}")
        body = response.read(131073)
        if len(body) > 131072:
            raise NotesError("GitHub Release response exceeds 128 KiB")
        data = json.loads(body)
    except (OSError, http.client.HTTPException, ValueError) as error:
        raise NotesError(f"cannot read GitHub Release for {tag}: {error}") from error
    finally:
        connection.close()
    if not isinstance(data, dict) or not isinstance(data.get("body"), str):
        raise NotesError(f"GitHub Release for {tag} has no reviewed body")
    return data["body"]


def release_main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--cmake", type=Path, default=Path("CMakeLists.txt"))
    parser.add_argument("--tag", required=True)
    args = parser.parse_args(argv)
    try:
        verify(args.snapshot.read_bytes(), args.tag, args.cmake.read_text(encoding="utf-8"), release_body(args.tag))
    except (OSError, NotesError) as error:
        print(f"[release-notes] {error}", file=sys.stderr)
        return 1
    return 0


# Pr

#!/usr/bin/env python3


MAX_RESPONSE_BYTES = 16 * 1024 * 1024
REPOSITORY = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,99}/[A-Za-z0-9][A-Za-z0-9_.-]{0,99}")
JIRA_KEY = re.compile(r"(?<![A-Z0-9])([A-Z][A-Z0-9]+-\d+)(?![A-Z0-9])")
CLOSING_REF = re.compile(
    r"\b(?:close[sd]?|fix(?:e[sd])?|resolve[sd]?)\s+(?:horocore/horo-engine)?#(\d+)\b",
    re.IGNORECASE,
)
ISSUE_REF = re.compile(r"(?<!\w)#(\d+)\b")


class AnnotationError(Exception):
    """A PR cannot be checked safely or a GitHub operation failed."""


async def cli_token() -> str:
    """Read CLI credentials with fixed arguments; no PR or user data enters argv."""
    process = await asyncio.create_subprocess_exec(
        "gh",
        "auth",
        "token",
        "--hostname",
        "github.com",
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.DEVNULL,
    )
    try:
        output, _ = await asyncio.wait_for(process.communicate(), timeout=30)
    except TimeoutError:
        process.kill()
        await process.communicate()
        raise AnnotationError("GitHub CLI credential lookup timed out") from None
    if process.returncode:
        raise AnnotationError("authenticate gh for github.com or set GH_TOKEN")
    return output.decode("utf-8").strip()


def github_token() -> str:
    """Resolve credentials once before workers start; never print token output."""
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if not token:
        try:
            token = asyncio.run(cli_token())
        except (OSError, UnicodeError) as error:
            raise AnnotationError("cannot read GitHub CLI credentials; set GH_TOKEN") from error
    if (
        not token
        or not token.isascii()
        or any(not 33 <= ord(character) < 127 for character in token)
    ):
        raise AnnotationError("GitHub credentials are empty or malformed")
    return token


def repository(value: str) -> str:
    """Accept only OWNER/REPO, never options, URLs, traversal or query fragments."""
    if REPOSITORY.fullmatch(value) is None:
        raise argparse.ArgumentTypeError("--repo must be an ASCII OWNER/REPO name")
    return value


class GitHub:
    """Fixed-host, bounded JSON transport; each worker owns its connection."""

    def __init__(self, token: str) -> None:
        """Retain credentials for this transport's fixed GitHub host."""
        self.token = token

    def request(self, method: str, path: str, payload: dict[str, Any] | None = None) -> Any:
        connection = http.client.HTTPSConnection("api.github.com", timeout=30)
        try:
            connection.request(
                method,
                path,
                body=json.dumps(payload) if payload is not None else None,
                headers={
                    "Accept": "application/vnd.github+json",
                    "Authorization": f"Bearer {self.token}",
                    "Content-Type": "application/json",
                    "User-Agent": "horo-pr-annotations",
                },
            )
            response = connection.getresponse()
            if not 200 <= response.status < 300:
                raise AnnotationError(f"GitHub request failed with HTTP {response.status}")
            body = response.read(MAX_RESPONSE_BYTES + 1)
            if len(body) > MAX_RESPONSE_BYTES:
                raise AnnotationError("GitHub response exceeds 16 MiB")
            return json.loads(body) if body else None
        except (OSError, http.client.HTTPException, ValueError) as error:
            raise AnnotationError("cannot read GitHub JSON response") from error
        finally:
            connection.close()

    def graphql(self, query: str, variables: dict[str, Any]) -> Any:
        result = self.request("POST", "/graphql", {"query": query, "variables": variables})
        if (
            not isinstance(result, dict)
            or result.get("errors")
            or not isinstance(result.get("data"), dict)
        ):
            raise AnnotationError("GitHub GraphQL request failed")
        return result["data"]

    def pages(self, path: str, params: dict[str, Any] | None = None) -> list[dict[str, Any]]:
        items: list[dict[str, Any]] = []
        for page in range(1, 1001):
            batch = self.request(
                "GET",
                path + "?" + urlencode({**(params or {}), "per_page": 100, "page": page}),
            )
            if not isinstance(batch, list):
                raise AnnotationError("GitHub paginated response must be an array")
            items.extend(batch)
            if len(batch) < 100:
                return items
        raise AnnotationError("GitHub pagination exceeds 1000 pages")


@dataclass
class CheckOptions:
    """Explicit shared check policy; transport creates a connection per request."""

    api: GitHub
    repo: str
    default_branch: str
    issue_override: int | None = None
    project: str | None = None
    project_number: int | None = None
    project_pr_numbers: set[int] = field(default_factory=set)
    include_development: bool = True
    fix: bool = False


def jira_key(pr: dict[str, Any]) -> str | None:
    text = " ".join((pr.get("headRefName") or "", pr.get("title") or "", pr.get("body") or ""))
    match = JIRA_KEY.search(text)
    return match.group(1) if match else None


def referenced_numbers(text: str, pattern: re.Pattern[str], excluded: int) -> set[int]:
    """Collect distinct references while excluding the PR's own number."""
    return {int(number) for number in pattern.findall(text)} - {excluded}


def issue_number(pr: dict[str, Any]) -> int | None:
    title_links = referenced_numbers(pr.get("title") or "", ISSUE_REF, pr["number"])
    if len(title_links) == 1:
        return next(iter(title_links))
    linked = {item["number"] for item in pr.get("closingIssuesReferences") or []}
    linked.update(referenced_numbers(pr.get("body") or "", CLOSING_REF, pr["number"]))
    if not linked:
        linked = title_links
    linked.discard(pr["number"])
    if len(linked) > 1:
        raise AnnotationError(f"multiple development issues: {sorted(linked)}")
    return next(iter(linked)) if linked else None


def search_jira_issue(pr: dict[str, Any], options: CheckOptions) -> int:
    """Resolve one exact Jira key without accepting partial or ambiguous searches."""
    key = jira_key(pr)
    if key is None:
        raise AnnotationError("no linked issue or Jira key in branch name")
    matches = options.api.request(
        "GET",
        "/search/issues?"
        + urlencode({"q": f"repo:{options.repo} is:issue {key}", "per_page": 100}),
    )
    if matches.get("incomplete_results") or matches["total_count"] > len(matches["items"]):
        raise AnnotationError("Jira issue search is incomplete; pass --issue NUMBER")
    matches = [
        item
        for item in matches["items"]
        if key in JIRA_KEY.findall(f"{item['title']} {item.get('body') or ''}")
    ]
    if len(matches) != 1:
        raise AnnotationError(
            f"Jira key {key} matched {len(matches)} GitHub issues; pass --issue NUMBER"
        )
    return matches[0]["number"]


def resolve_issue(pr: dict[str, Any], options: CheckOptions) -> dict[str, Any]:
    number = options.issue_override or issue_number(pr)
    if number is None:
        number = search_jira_issue(pr, options)
    result = options.api.request("GET", f"/repos/{options.repo}/issues/{number}")
    if "pull_request" in result:
        raise AnnotationError("development reference points to a pull request, not an issue")
    return {
        "id": result["node_id"],
        "number": result["number"],
        "title": result["title"],
        "body": result.get("body") or "",
        "milestone": result.get("milestone"),
        "labels": result.get("labels") or [],
        "assignees": result.get("assignees") or [],
        "projectItems": [],
    }


def names(items: list[dict[str, Any]] | None, field: str) -> set[str]:
    return {item[field] for item in items or []}


def closing_reference_page(data: dict[str, Any]) -> dict[str, Any]:
    """Require GitHub to return a closing-reference connection for the PR."""
    node = data.get("node")
    if not node or not node.get("closingIssuesReferences"):
        raise AnnotationError("GitHub did not return PR closing references")
    return node["closingIssuesReferences"]


def has_development_link(
    pr: dict[str, Any], issue_number_value: int, options: CheckOptions
) -> bool:
    if pr.get("baseRefName") == options.default_branch and issue_number_value in referenced_numbers(
        pr.get("body") or "", CLOSING_REF, pr["number"]
    ):
        return True
    cursor = None
    seen_cursors: set[str] = set()
    for _ in range(1000):
        data = options.api.graphql(
            "query($id:ID!,$after:String){ node(id:$id){ ... on PullRequest { "
            "closingIssuesReferences(first:100,after:$after){ nodes{number} pageInfo{hasNextPage endCursor} } } } }",
            {"id": pr["id"], "after": cursor},
        )
        result = closing_reference_page(data)
        if issue_number_value in {item["number"] for item in result["nodes"]}:
            return True
        if not result["pageInfo"]["hasNextPage"]:
            return False
        next_cursor = result["pageInfo"]["endCursor"]
        if not next_cursor or next_cursor in seen_cursors:
            raise AnnotationError("GitHub closing-reference pagination did not advance")
        seen_cursors.add(next_cursor)
        cursor = next_cursor
    raise AnnotationError("GitHub closing-reference pagination exceeds 1000 pages")


def milestone_changes(
    pr: dict[str, Any], issue: dict[str, Any], default_branch: str
) -> tuple[list[str], list[tuple[str, str]]]:
    if issue.get("number") is None:
        if pr.get("baseRefName") == default_branch and CLOSING_REF.findall(pr.get("body") or ""):
            return [
                "multiple issues referenced; milestone is ambiguous; closing references are present"
            ], []
        return ["no unambiguous issue; milestone and Development skipped"], []
    expected = issue.get("milestone")
    if expected is None:
        return ["issue has no milestone; milestone skipped"], []
    if (pr.get("milestone") or {}).get("number") != expected["number"]:
        return [], [("milestone", expected["title"])]
    return [], []


def changes(
    pr: dict[str, Any],
    issue: dict[str, Any],
    options: CheckOptions,
) -> tuple[list[str], list[tuple[str, str]]]:
    notes, fixes = milestone_changes(pr, issue, options.default_branch)
    issue_number_value = issue.get("number")
    for label in sorted(names(issue.get("labels"), "name") - names(pr.get("labels"), "name")):
        fixes.append(("label", label))
    current_projects = names(pr.get("projectItems"), "title")
    expected_projects = names(issue.get("projectItems"), "title")
    if options.project:
        expected_projects.add(options.project)
    for project_name in sorted(expected_projects - current_projects):
        fixes.append(("project", project_name))
    issue_assignees = names(issue.get("assignees"), "login")
    expected_assignees = issue_assignees or {pr["author"]["login"]}
    for assignee in sorted(expected_assignees - names(pr.get("assignees"), "login")):
        fixes.append(("assignee", assignee))
    if (
        issue_number_value is not None
        and options.include_development
        and not has_development_link(pr, issue_number_value, options)
    ):
        fixes.append(("development", str(issue_number_value)))
    return notes, fixes


def apply_fix(
    number: int,
    kind: str,
    value: str,
    pr: dict[str, Any],
    issue: dict[str, Any],
    options: CheckOptions,
) -> None:
    if kind == "development":
        options.api.graphql(
            "mutation($issue:ID!,$prs:[ID!]!){ addCloseIssueReferences(input:{issueId:$issue,pullRequestIds:$prs}){issue{id}} }",
            {"issue": issue["id"], "prs": [pr["id"]]},
        )
        return
    if kind == "milestone":
        options.api.request(
            "PATCH",
            f"/repos/{options.repo}/issues/{number}",
            {"milestone": issue["milestone"]["number"]},
        )
    elif kind == "label":
        options.api.request(
            "POST", f"/repos/{options.repo}/issues/{number}/labels", {"labels": [value]}
        )
    elif kind == "assignee":
        options.api.request(
            "POST",
            f"/repos/{options.repo}/issues/{number}/assignees",
            {"assignees": [value]},
        )
    elif kind == "project":
        if options.project_number is None:
            raise AnnotationError("adding an item requires --project NUMBER")
        owner = options.repo.split("/", 1)[0]
        options.api.request(
            "POST",
            f"/orgs/{owner}/projectsV2/{options.project_number}/items",
            {"type": "PullRequest", "id": pr["databaseId"]},
        )
    else:
        raise AnnotationError(f"unknown annotation kind: {kind}")


def load_pr(number: int, options: CheckOptions) -> dict[str, Any]:
    raw_pr = options.api.request("GET", f"/repos/{options.repo}/pulls/{number}")
    return {
        "id": raw_pr["node_id"],
        "databaseId": raw_pr["id"],
        "number": raw_pr["number"],
        "title": raw_pr["title"],
        "body": raw_pr.get("body") or "",
        "headRefName": raw_pr["head"]["ref"],
        "baseRefName": raw_pr["base"]["ref"],
        "author": {"login": raw_pr["user"]["login"]},
        "assignees": raw_pr.get("assignees") or [],
        "closingIssuesReferences": [],
        "milestone": raw_pr.get("milestone"),
        "labels": raw_pr.get("labels") or [],
        "projectItems": (
            [{"title": options.project}]
            if number in options.project_pr_numbers and options.project
            else []
        ),
        "url": raw_pr["html_url"],
    }


def report_changes(
    number: int,
    pr: dict[str, Any],
    issue: dict[str, Any],
    fixes: list[tuple[str, str]],
    options: CheckOptions,
) -> bool:
    """Report missing fields or apply them in order, stopping at the first failed write."""
    action = "fix" if options.fix else "missing"
    for kind, value in fixes:
        print(f"  {action}: {kind} {value}", flush=True)
        if options.fix:
            try:
                apply_fix(number, kind, value, pr, issue, options)
            except AnnotationError as error:
                print(f"  ERROR: {kind}: {error}", file=sys.stderr, flush=True)
                return False
    return True


def report_notes(notes: list[str]) -> None:
    """Report skipped annotation decisions before any attempted writes."""
    for note in notes:
        print(f"  note: {note}", flush=True)


def check_pr(number: int, options: CheckOptions) -> bool:
    pr = load_pr(number, options)
    try:
        issue = resolve_issue(pr, options)
        issue_label = f"issue #{issue['number']}"
    except AnnotationError as error:
        issue = {
            "id": "",
            "number": None,
            "title": "",
            "body": "",
            "milestone": None,
            "labels": [],
            "assignees": [],
            "projectItems": [],
        }
        issue_label = "no unambiguous issue"
        print(f"PR #{number}: note: {error}", flush=True)
    notes, fixes = changes(pr, issue, options)
    print(f"PR #{number} -> {issue_label} ({jira_key(pr) or 'no Jira key'})", flush=True)
    report_notes(notes)
    if options.fix and issue["number"] is None:
        return False
    if not report_changes(number, pr, issue, fixes, options):
        return False
    if not fixes and issue["number"] is not None:
        print("  OK", flush=True)
    return issue["number"] is not None and (not fixes or options.fix)


def list_pr_numbers(options: CheckOptions, state: str, limit: int | None) -> list[int]:
    api_state = state
    if state == "merged":
        api_state = "closed"
    pulls = options.api.pages(f"/repos/{options.repo}/pulls", {"state": api_state})
    if state in {"merged", "closed"}:
        pulls = [pr for pr in pulls if bool(pr.get("merged_at")) == (state == "merged")]
    numbers = [pr["number"] for pr in pulls]
    return numbers[:limit] if limit is not None else numbers


def validate_selection(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    """Require one selection mode and a single PR for an explicit issue override."""
    if args.all == bool(args.prs) or (args.issue and len(args.prs) != 1):
        parser.error("provide PR numbers or --all; --issue requires exactly one PR")


def validate_arguments(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    """Reject ambiguous selections and invalid execution bounds before API access."""
    validate_selection(parser, args)
    if args.limit is not None and args.limit < 1:
        parser.error("--limit must be positive")
    if not 1 <= args.workers <= 16:
        parser.error("--workers must be between 1 and 16")
    if args.issue is not None and args.issue < 1:
        parser.error("--issue must be positive")
    if any(number < 1 for number in args.prs):
        parser.error("PR numbers must be positive")


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prs", nargs="*", type=int, metavar="PR", help="PR numbers")
    parser.add_argument(
        "--repo", type=repository, default="horocore/horo-engine", help="OWNER/REPO"
    )
    parser.add_argument("--project", help="also require this GitHub project number or title")
    parser.add_argument(
        "--issue",
        type=int,
        help="issue number for one PR if automatic lookup is ambiguous",
    )
    parser.add_argument("--all", action="store_true", help="check every PR, including merged PRs")
    parser.add_argument("--state", choices=("open", "closed", "merged", "all"), default="all")
    parser.add_argument(
        "--limit", type=int, help="optional maximum number of PRs to process with --all"
    )
    parser.add_argument("--workers", type=int, default=8, help="parallel PR checks (default: 8)")
    parser.add_argument("--fix", action="store_true", help="write missing/incorrect annotations")
    parser.add_argument(
        "--skip-development",
        action="store_true",
        help="skip Development links when GitHub GraphQL is unavailable",
    )
    args = parser.parse_args(argv)
    validate_arguments(parser, args)
    return args


def load_options(args: argparse.Namespace) -> CheckOptions:
    api = GitHub(github_token())
    default_branch = api.request("GET", f"/repos/{args.repo}")["default_branch"]
    options = CheckOptions(
        api,
        args.repo,
        default_branch,
        issue_override=args.issue,
        project=args.project,
        include_development=not args.skip_development,
        fix=args.fix,
    )
    if options.project and options.project.isdecimal():
        options.project_number = int(options.project)
        if options.project_number < 1:
            raise AnnotationError("--project number must be positive")
        owner = options.repo.split("/", 1)[0]
        path = f"/orgs/{owner}/projectsV2/{options.project_number}"
        options.project = api.request("GET", path)["title"]
        options.project_pr_numbers = {
            item["content"]["number"]
            for item in api.pages(path + "/items")
            if (item.get("content") or {}).get("node_id", "").startswith("PR_")
        }
    return options


def process_pr(number: int, options: CheckOptions) -> bool:
    try:
        return check_pr(number, options)
    except AnnotationError as error:
        print(f"PR #{number}: ERROR: {error}", file=sys.stderr)
        return False


def pr_main(argv=None) -> int:
    args = parse_args(argv)
    try:
        options = load_options(args)
        prs = args.prs
        if args.all:
            prs = list_pr_numbers(options, args.state, args.limit)
        success = True
        with ThreadPoolExecutor(max_workers=args.workers) as executor:
            futures = [executor.submit(process_pr, number, options) for number in prs]
            for future in as_completed(futures):
                success = future.result() and success
        return 0 if success else 1
    except (AnnotationError, json.JSONDecodeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


# Evidence

EVIDENCE_WORKSPACE_EVIDENCE_ERROR = "Qualification evidence must remain inside the current workspace"


def evidence_workspace_path(value):
    """Reject traversal and resolved symlink escapes for both input and output evidence."""
    candidate = Path(value).resolve()
    try:
        candidate.relative_to(Path.cwd().resolve())
    except ValueError as error:
        raise argparse.ArgumentTypeError(EVIDENCE_WORKSPACE_EVIDENCE_ERROR) from error
    return candidate


def parse_evidence_arguments(description, *, argv=None, families=()):
    """Preserve the common discovery/JUnit CLI and require an explicit discovery output."""
    parser = argparse.ArgumentParser(description=description)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--discovery", type=evidence_workspace_path)
    mode.add_argument("--output-junit", type=evidence_workspace_path)
    parser.add_argument("--regex-output", type=evidence_workspace_path)
    if families:
        parser.add_argument("--family", choices=families, default=families[0])
    arguments = parser.parse_args(argv)
    if arguments.discovery is not None and arguments.regex_output is None:
        parser.error("--discovery requires --regex-output")
    return arguments


def write_case_regex(path, cases):
    """Recheck the actual sink, including direct callers and paths changed since argument parsing."""
    destination = Path(path).resolve()
    if not destination.is_relative_to(Path.cwd().resolve()):
        raise ValueError(EVIDENCE_WORKSPACE_EVIDENCE_ERROR)
    # CTest uses POSIX extended syntax, without Python noncapturing groups.
    pattern = "^(" + "|".join(re.escape(name) for name in sorted(cases)) + ")$"
    destination.write_text(pattern, encoding="utf-8")


# Scene

#!/usr/bin/env python3


# Resolve the trusted sibling identically for direct CLI and file-spec loading.


SCENE_WORKSPACE_EVIDENCE_ERROR = "Qualification evidence must remain inside the current workspace"

SCENE_PREFIX = "HoroSceneRuntimeConversionTests::"
SCENE_CASES = frozenset(SCENE_PREFIX + name for name in (
    "Expansion allocation failure never evicts the prior exact immutable candidate",
    "Scene encoding admission reports each allocation failure without accepting invalid authored bytes",
    "Canonical serialization survives every production allocation failure",
    "Canonical moves retire every wire alternative without allocation - OrderedWireValue",
    "Canonical moves retire every wire alternative without allocation - SortedWireValue",
    "Resolver admission computes actual immutable evidence transactionally under every allocation failure",
    "Recursive canonical copies preserve source and destination at every allocation failure - OrderedWireValue",
    "Recursive canonical copies preserve source and destination at every allocation failure - SortedWireValue",
    "Cache pressure after validated worker completion does not discard complete Scene output",
    "Cancellation during completion memoization cannot publish a validated definition",
))
NAVIGATION_PREFIX = "HoroNavigationProjectSettingsTests::"
NAVIGATION_PHASES = {
    NAVIGATION_PREFIX + "Every navigation commit allocation failure precedes all owner publication": "commit",
    NAVIGATION_PREFIX + "Every navigation grouped replay allocation failure preserves all owner histories": "replay",
    NAVIGATION_PREFIX + "Scene decoding releases completed DOM without allocating during retirement": "scene",
}
NAVIGATION_CASES = frozenset(NAVIGATION_PHASES)


COLLISION_PHASES = {
    "HoroProjectCollisionSettingsTests::Native registry inventory recovers after every one-shot allocation failure": "inventory",
    "HoroProjectCollisionSettingsTests::Collision path admission propagates every allocation failure before owner publication": "paths",
    "HoroProjectCollisionSettingsTests::Collision history publication exhaustively rolls back every one-shot allocation failure": "commit",
    "HoroProjectCollisionSettingsTests::Canonical Scene decoder unwinds every single allocation failure without terminating completed DOM cleanup": "scene",
}
SIDECAR_CASE = ("HoroProjectCollisionSettingsTests::Canonical sidecar decoding unwinds every allocation "
                "across valid invalid and deep compound DOMs")
SIDECAR_PHASES = ("valid", "duplicate", "malformed", "deep")
COLLISION_CASES = frozenset((*COLLISION_PHASES, SIDECAR_CASE))


def scene_verify_discovery(document, cases=SCENE_CASES):
    """Require every exact case and refuse a disabled replacement for executable evidence."""
    tests = document.get("tests", [])
    families = {name.split(" - ", 1)[0] for name in cases}
    selected = [test for test in tests if any(
        test.get("name") == family or test.get("name", "").startswith(family + " - ")
        for family in families
    )]
    names = [test["name"] for test in selected]
    if len(names) != len(cases) or set(names) != cases:
        raise ValueError(f"Scene allocation discovery differs: missing={sorted(cases - set(names))}")
    for test in selected:
        if any(prop.get("name") == "DISABLED" and prop.get("value") for prop in test.get("properties", [])):
            raise ValueError(f"Scene allocation test is disabled: {test['name']}")

def scene_verify_junit(path, cases=SCENE_CASES):
    """Count actual cases rather than assertions and reject every skipped or failed execution."""
    from defusedxml import ElementTree
    from defusedxml.common import DefusedXmlException
    path = Path(path).resolve()
    if not path.is_relative_to(Path.cwd().resolve()):
        raise ValueError(SCENE_WORKSPACE_EVIDENCE_ERROR)
    root = ElementTree.parse(path).getroot()
    executed = list(root.iter("testcase"))
    names = [case.get("name") for case in executed]
    if len(executed) != len(cases) or set(names) != cases:
        raise ValueError(f"Scene allocation execution differs: expected {len(cases)}, got {len(executed)}")
    for case in executed:
        failed = any(case.find(tag) is not None for tag in ("failure", "error", "skipped"))
        if failed or case.get("status") in ("notrun", "disabled"):
            raise ValueError(f"Scene allocation test did not pass: {case.get('name')}")
    if cases == NAVIGATION_CASES:
        verify_navigation_positions(executed)
    for suite in root.iter("testsuite"):
        for field in ("failures", "errors", "skipped", "disabled"):
            if int(suite.get(field, "0")):
                raise ValueError(f"Scene allocation suite has nonzero {field}")


def verify_navigation_positions(executed):
    """Require completed evidence for every measured allocation, without a platform-specific upper bound."""
    for case in executed:
        if case.get("status") != "run":
            raise ValueError(f"Navigation allocation case did not execute: {case.get('name')}")
        phase = NAVIGATION_PHASES[case.get("name")]
        pattern = rf"\[navigation-allocation\] {phase} measured=(\d+) completed=(\d+)"
        markers = re.findall(pattern, "".join(case.itertext()))
        if len(markers) != 1 or int(markers[0][0]) == 0 or markers[0][0] != markers[0][1]:
            raise ValueError(f"Navigation fault positions incomplete: {phase}")


def scene_write_regex(path, cases=SCENE_CASES):
    """Write only resolved workspace evidence, including for direct non-CLI callers."""
    write_case_regex(path, cases)


def verify_collision_positions(path):
    """Require proof of every measured failure position, including probes above Debug-sized budgets."""
    from defusedxml import ElementTree
    from defusedxml.common import DefusedXmlException
    scene_verify_junit(path, COLLISION_CASES)
    for case in ElementTree.parse(path).getroot().iter("testcase"):
        sidecar = case.get("name") == SIDECAR_CASE
        phases = SIDECAR_PHASES if sidecar else (COLLISION_PHASES[case.get("name")],)
        prefix = "collision-sidecar-allocation" if sidecar else "collision-allocation"
        output = "".join(case.itertext())
        for phase in phases:
            positions = re.findall(r"\[" + prefix + r"\] " + phase + r" measured=(\d+) completed=(\d+)", output)
            if case.get("status") != "run" or len(positions) != 1:
                raise ValueError("Collision allocation execution has no unique complete position proof")
            measured, completed = map(int, positions[0])
            if measured == 0 or completed != measured:
                raise ValueError("Collision allocation sweep did not execute every measured position")


def scene_main(argv=None):
    """Validate discovery before execution, or validate the resulting exact Release JUnit."""
    args = parse_evidence_arguments(__doc__, argv=argv, families=("scene", "navigation", "collision"))
    cases = {"scene": SCENE_CASES, "navigation": NAVIGATION_CASES, "collision": COLLISION_CASES}[args.family]
    if args.discovery:
        scene_verify_discovery(json.loads(args.discovery.read_text(encoding="utf-8-sig")), cases)
        scene_write_regex(args.regex_output, cases)
    else:
        if args.family == "collision":
            verify_collision_positions(args.output_junit)
        else:
            scene_verify_junit(args.output_junit, cases)
        print(f"{args.family} allocation qualification: {len(cases)} cases passed; zero failures, errors or skips.")


# Character

#!/usr/bin/env python3


# Resolve the trusted sibling identically for direct CLI and file-spec loading.


CHARACTER_WORKSPACE_EVIDENCE_ERROR = "Qualification evidence must remain inside the current workspace"
CHARACTER_PREFIX = "HoroPhysicsTests::"
CHARACTER_CASES = frozenset({CHARACTER_PREFIX + "Character preparation allocation failures roll back every retained storage stage"})


def character_verify_discovery(document):
    """Require every exact case and refuse a disabled replacement for executable evidence."""
    tests = document.get("tests", [])
    families = {name.split(" - ", 1)[0] for name in CHARACTER_CASES}
    selected = [test for test in tests if any(
        test.get("name") == family or test.get("name", "").startswith(family + " - ")
        for family in families
    )]
    names = [test["name"] for test in selected]
    if len(names) != len(CHARACTER_CASES) or set(names) != CHARACTER_CASES:
        raise ValueError(f"Character allocation discovery differs: missing={sorted(CHARACTER_CASES - set(names))}")
    for test in selected:
        if any(prop.get("name") == "DISABLED" and prop.get("value") for prop in test.get("properties", [])):
            raise ValueError(f"Character allocation test is disabled: {test['name']}")


def character_verify_junit(path):
    """Count actual cases rather than assertions and reject every skipped or failed execution."""
    from defusedxml import ElementTree as element_tree
    from defusedxml.common import DefusedXmlException
    path = Path(path).resolve()
    if not path.is_relative_to(Path.cwd().resolve()):
        raise ValueError(CHARACTER_WORKSPACE_EVIDENCE_ERROR)
    root = element_tree.parse(path).getroot()
    cases = list(root.iter("testcase"))
    names = [case.get("name") for case in cases]
    if len(cases) != len(CHARACTER_CASES) or set(names) != CHARACTER_CASES:
        raise ValueError(f"Character allocation execution differs: expected {len(CHARACTER_CASES)}, got {len(cases)}")
    for case in cases:
        failed = any(case.find(tag) is not None for tag in ("failure", "error", "skipped"))
        if failed or case.get("status") in ("notrun", "disabled"):
            raise ValueError(f"Character allocation test did not pass: {case.get('name')}")
    for suite in root.iter("testsuite"):
        for field in ("failures", "errors", "skipped", "disabled"):
            if int(suite.get(field, "0")):
                raise ValueError(f"Character allocation suite has nonzero {field}")


def character_write_regex(path):
    """Write only resolved workspace evidence, including for direct non-CLI callers."""
    write_case_regex(path, CHARACTER_CASES)


def character_main(argv=None):
    """Validate discovery before execution, or validate the resulting exact Release JUnit."""
    args = parse_evidence_arguments(__doc__, argv=argv)
    if args.discovery:
        character_verify_discovery(json.loads(args.discovery.read_text(encoding="utf-8-sig")))
        character_write_regex(args.regex_output)
    else:
        character_verify_junit(args.output_junit)
        print(f"Character allocation qualification: {len(CHARACTER_CASES)} cases passed; zero failures, errors or skips.")


# Extension

#!/usr/bin/env python3


# Resolve the trusted sibling identically for direct CLI and file-spec loading.


EXTENSION_PREFIX = "HoroExtensionManagerAllocationTests::"
WORKSPACE_CASE = "HoroWorkspaceLayoutPersistenceTests::Split parsing releases every owned allocation when any allocation fails"
EXTENSION_CASES = {
    WORKSPACE_CASE:
        ("workspace-allocation", 1),
    EXTENSION_PREFIX + "Every recovery admission allocation failure precedes native callbacks and preserves independent status":
        ("recovery-admission-allocation", 1),
    EXTENSION_PREFIX + "Completed manifest DOM retirement preserves every allocation failure on valid and rejected schemas":
        ("manifest-allocation", 3),
}


def extension_verify_discovery(document):
    """Require all exact enabled executable cases before CTest filtering."""
    selected = [test for test in document.get("tests", [])
                if test.get("name", "").startswith(EXTENSION_PREFIX) or test.get("name") == WORKSPACE_CASE]
    if len(selected) != len(EXTENSION_CASES) or {test["name"] for test in selected} != set(EXTENSION_CASES):
        raise ValueError("Extension allocation discovery must contain all exact cases")
    for test in selected:
        if any(prop.get("name") == "DISABLED" and prop.get("value") for prop in test.get("properties", [])):
            raise ValueError(f"Extension allocation case is disabled: {test['name']}")


def verify_case(case):
    """Validate one actual successful case and every required nonzero completed sweep."""
    if any(case.find(tag) is not None for tag in ("failure", "error", "skipped")) or case.get("status") in ("notrun", "disabled"):
        raise ValueError(f"Extension allocation case did not pass: {case.get('name')}")
    marker, expected = EXTENSION_CASES[case.get("name")]
    output = "".join(case.itertext())
    if marker == "workspace-allocation":
        measured = re.findall(r"\[workspace-allocation\] measured=(\d+)", output)
        completed = re.findall(r"\[workspace-allocation\] completed=(\d+)", output)
        if len(measured) != expected or len(completed) != expected:
            raise ValueError("Workspace allocation full-sweep evidence is missing or duplicated")
        counts = list(zip(measured, completed))
    else:
        counts = re.findall(r"\[" + re.escape(marker) + r"\] measured=(\d+) completed=(\d+)", output)
    if len(counts) != expected or any(int(measured) == 0 or measured != completed for measured, completed in counts):
        raise ValueError(f"Extension allocation full-sweep evidence differs: {case.get('name')}")


def extension_verify_junit(path):
    """Reject absent execution, failed/skipped cases, or incomplete/nonmeasured sweeps."""
    from defusedxml import ElementTree as element_tree
    from defusedxml.common import DefusedXmlException
    root = element_tree.parse(path).getroot()
    cases = list(root.iter("testcase"))
    if len(cases) != len(EXTENSION_CASES) or {case.get("name") for case in cases} != set(EXTENSION_CASES):
        raise ValueError("Extension allocation execution must contain all exact cases")
    for case in cases:
        verify_case(case)
    for suite in root.iter("testsuite"):
        if any(int(suite.get(field, "0")) for field in ("failures", "errors", "skipped", "disabled")):
            raise ValueError("Extension allocation suite contains failures, errors or skips")


def extension_main(argv=None):
    """Verify discovery before execution, or validate the exact Release JUnit afterward."""
    args = parse_evidence_arguments(__doc__, argv=argv)
    if args.discovery:
        extension_verify_discovery(json.loads(args.discovery.read_text(encoding="utf-8-sig")))
        write_case_regex(args.regex_output, EXTENSION_CASES)
    else:
        extension_verify_junit(args.output_junit)
        print("Extension allocation qualification: three cases and all five nonzero measured sweeps completed; zero failures, errors or skips.")


# Save

SAVE_CASES = {
    "Lifecycle allocation failure after visibility preserves publication knowledge": "lifecycle",
    "Retention allocation failure after selection preserves publication and binding ownership": "retention",
}
SAVE_PREFIX = "HoroRuntimeSaveSlotLifecycleTests::"


def verify_report(path: Path) -> None:
    """Reject missing, skipped, failed or incompletely exercised allocation cases."""
    import defusedxml.ElementTree as etree
    report = path.resolve()
    if not report.is_relative_to(Path.cwd().resolve()):
        raise ValueError("Qualification report must be inside the current workspace")
    data = report.read_bytes()
    root = etree.fromstring(data, forbid_dtd=True)
    if root.tag != "testsuite" or any(root.get(key) != value for key, value in
                                     (("tests", "2"), ("failures", "0"), ("skipped", "0"))):
        raise ValueError("Qualification suite must report two tests with zero failures and skips")
    if root.get("errors", "0") != "0" or root.get("disabled", "0") != "0":
        raise ValueError("Qualification suite contains errors or disabled cases")
    cases = list(root.iter("testcase"))
    expected = {SAVE_PREFIX + name for name in SAVE_CASES}
    if len(cases) != 2 or {case.get("name") for case in cases} != expected:
        raise ValueError("Qualification requires exactly the two named Save allocation cases")
    for case in cases:
        if case.get("status") != "run" or any(case.find(tag) is not None for tag in ("skipped", "failure", "error")):
            raise ValueError(f"Qualification case did not pass: {case.get('name')}")
        phase = SAVE_CASES[case.get("name", "").removeprefix(SAVE_PREFIX)]
        output = "".join(case.itertext())
        require_variants(output, phase)


def require_variants(output: str, phase: str) -> None:
    """Require both generator paths through recovery and completed fixture cleanup."""
    markers = []
    for variant in ("success", "failure"):
        markers.extend((f"{phase}-sync-{variant}", f"{phase}-failure-reset",
                        f"{phase}-reopen-returned", f"{phase}-reconcile-returned",
                        f"{phase}-fixture-cleanup-next", "fixture-scope-exited"))
    cursor = 0
    for marker in markers:
        literal = f"[save-allocation] {marker}"
        position = output.find(literal, cursor)
        if position < 0:
            raise ValueError(f"Incomplete {phase} variant evidence: {marker}")
        cursor = position + len(literal)


def save_workspace_path(value: str) -> Path:
    """Keep CLI report reads inside the caller's explicit workspace, including symlink resolution."""
    root = Path.cwd().resolve()
    candidate = Path(value).resolve()
    try:
        candidate.relative_to(root)
    except ValueError as error:
        raise argparse.ArgumentTypeError("Qualification report must be inside the current workspace") from error
    return candidate


def save_main(argv=None) -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=save_workspace_path)
    args = parser.parse_args(argv)
    verify_report(args.report)
    print("Save allocation qualification: 2 passed cases, both variants completed, zero skips")


# Dev

#!/usr/bin/env python3


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ENV_FILE_NAME = ".env.local"
DEFAULT_ENV_FILE = REPOSITORY_ROOT / DEFAULT_ENV_FILE_NAME
DEFAULT_BUILD_DIRECTORY = REPOSITORY_ROOT / "build" / "dev-editor"
DEFAULT_CI_BUILD_DIRECTORY = REPOSITORY_ROOT / "build" / "ci"
DEFAULT_OTLP_ENDPOINT = "http://127.0.0.1:4318"
_ENVIRONMENT_KEY = re.compile(r"^[A-Za-z_]\w*$", re.ASCII)

# Diagnostic Item String Constants (Sonar S1192)
_TOOLCHAIN_COMPILER_NAME = "C++ Compiler"
_TOOLCHAIN_CMAKE_NAME = "CMake"
_TOOLCHAIN_NINJA_NAME = "Ninja Generator"
_TOOLCHAIN_CLANG_FORMAT_NAME = "clang-format"
_TOOLCHAIN_CCACHE_NAME = "ccache"
_PYTHON_PYTEST_NAME = "pytest Module"
_GRAPHICS_DISPLAY_NAME = "Interactive Display"
_OBSERVABILITY_OTLP_NAME = "OTLP Collector"
_SONAR_TOKEN_ENV_KEY = "HORO_SONAR_TOKEN"
_SONAR_DEFAULT_COMPILE_COMMANDS = "build/ci/compile_commands.json"

_CPP_EXTENSIONS = {".h", ".hpp", ".cpp", ".c", ".cc", ".cxx", ".mm", ".m"}
_EXCLUDED_PATH_PARTS = {"vendor", "build", "deprecated", ".horo", ".venv"}


class ConfigurationError(ValueError):
    """Raised when the local developer configuration is invalid."""


class CheckStatus(Enum):
    """Status level for an environment check item."""

    OK = "OK"
    WARN = "WARN"
    ERROR = "ERROR"
    INFO = "INFO"


@dataclass(frozen=True)
class Endpoint:
    """Validated OTLP base endpoint and its TCP reachability address."""

    url: str
    host: str
    port: int


@dataclass(frozen=True)
class DeveloperSettings:
    """Resolved settings consumed only by this developer runner."""

    otel_export_enabled: bool
    endpoint: Endpoint


@dataclass
class DoctorItem:
    """Single diagnostic report entry."""

    category: str
    name: str
    status: CheckStatus
    message: str
    details: str | None = None


@dataclass
class DoctorReport:
    """Aggregated doctor diagnostic report."""

    items: list[DoctorItem] = field(default_factory=list)

    def add(
        self,
        category: str,
        name: str,
        status: CheckStatus,
        message: str,
        details: str | None = None,
    ) -> None:
        """Append an item to the report."""
        self.items.append(DoctorItem(category, name, status, message, details))

    @property
    def has_errors(self) -> bool:
        """Return true if any critical check failed."""
        return any(item.status == CheckStatus.ERROR for item in self.items)

    @property
    def has_warnings(self) -> bool:
        """Return true if any warning check occurred."""
        return any(item.status == CheckStatus.WARN for item in self.items)

    def to_dict(self) -> dict[str, object]:
        """Serialize report to a plain dictionary for JSON export."""
        return {
            "has_errors": self.has_errors,
            "has_warnings": self.has_warnings,
            "checks": [
                {
                    "category": item.category,
                    "name": item.name,
                    "status": item.status.value,
                    "message": item.message,
                    "details": item.details,
                }
                for item in self.items
            ],
        }

    def _render_summary(self, use_color: bool) -> str:
        """Format the summary line based on error and warning state."""
        if self.has_errors:
            return (
                "\033[31mStatus: Critical issues found. Please address the errors above.\033[0m"
                if use_color
                else "Status: Critical issues found. Please address the errors above."
            )
        if self.has_warnings:
            return (
                "\033[33mStatus: Ready with warnings.\033[0m"
                if use_color
                else "Status: Ready with warnings."
            )
        return (
            "\033[32mStatus: All development environment checks passed!\033[0m"
            if use_color
            else "Status: All development environment checks passed!"
        )

    def render(self, use_color: bool = True) -> str:
        """Format the report into a clean, human-readable terminal string."""
        encoding = getattr(sys.stdout, "encoding", "") or ""
        supports_unicode = "utf" in encoding.lower()

        ok_sym = "[✓]" if supports_unicode else "[OK]"
        warn_sym = "[!]"
        err_sym = "[✗]" if supports_unicode else "[FAIL]"
        info_sym = "[i]"

        symbols = {
            CheckStatus.OK: (f"\033[32m{ok_sym}\033[0m" if use_color else ok_sym),
            CheckStatus.WARN: (f"\033[33m{warn_sym}\033[0m" if use_color else warn_sym),
            CheckStatus.ERROR: (f"\033[31m{err_sym}\033[0m" if use_color else err_sym),
            CheckStatus.INFO: (f"\033[36m{info_sym}\033[0m" if use_color else info_sym),
        }

        lines: list[str] = ["Horo Developer Doctor Report", "============================", ""]
        current_category: str | None = None

        for item in self.items:
            if item.category != current_category:
                current_category = item.category
                lines.append(f"== {current_category} ==")

            sym = symbols.get(item.status, "[?]")
            line = f" {sym} {item.name}: {item.message}"
            if item.details:
                line += f" ({item.details})"
            lines.append(line)

        lines.append("")
        lines.append(self._render_summary(use_color))
        return "\n".join(lines)


def _parse_dotenv_line(path: Path, line_number: int, line: str) -> tuple[str, str] | None:
    """Parse one single line from a dotenv file."""
    if not line or line.startswith("#"):
        return None
    if line.startswith("export ") or "=" not in line:
        raise ConfigurationError(f"{path}:{line_number}: expected KEY=value")
    key, raw_value = line.split("=", 1)
    if not _ENVIRONMENT_KEY.fullmatch(key):
        raise ConfigurationError(f"{path}:{line_number}: invalid environment key {key!r}")
    value = raw_value.strip()
    if "$(" in value or "${" in value or "`" in value:
        raise ConfigurationError(f"{path}:{line_number}: environment expansion and commands are not supported")
    if value.startswith(("\"", "'")):
        if len(value) < 2 or value[-1] != value[0]:
            raise ConfigurationError(f"{path}:{line_number}: unterminated quoted value")
        return key, value[1:-1]
    if any(character.isspace() for character in value):
        raise ConfigurationError(f"{path}:{line_number}: values containing whitespace must be quoted")
    return key, value


def parse_dotenv(path: Path) -> dict[str, str]:
    """Parse the intentionally small, non-executable developer dotenv grammar."""
    if not path.exists():
        return {}

    values: dict[str, str] = {}
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        parsed = _parse_dotenv_line(path, line_number, raw_line.strip())
        if parsed is not None:
            key, value = parsed
            if key in values:
                raise ConfigurationError(f"{path}:{line_number}: duplicate environment key {key!r}")
            values[key] = value
    return values


def parse_enabled(value: str, setting_name: str = "HORO_DEV_OTEL_EXPORT") -> bool:
    """Parse the explicit ON/OFF developer switch."""
    normalized = value.strip().upper()
    if normalized == "ON":
        return True
    if normalized == "OFF":
        return False
    raise ConfigurationError(f"{setting_name} must be ON or OFF")


def _validate_url_structure(parsed: ParseResult, setting_name: str) -> None:
    """Validate protocol and general URL constraints."""
    if parsed.scheme not in {"http", "https"}:
        raise ConfigurationError(f"{setting_name} must use http or https")
    if parsed.username is not None or parsed.password is not None:
        raise ConfigurationError(f"{setting_name} must not include credentials")
    if parsed.path not in {"", "/"} or parsed.params or parsed.query or parsed.fragment:
        raise ConfigurationError(f"{setting_name} must be a base URL without a path, query, or fragment")


def validate_endpoint(value: str) -> Endpoint:
    """Validate an OTLP HTTP base URL without sending protocol traffic."""
    if any(character.isspace() for character in value) or "?" in value or "#" in value:
        raise ConfigurationError("HORO_OTEL_ENDPOINT must not contain whitespace, a query, or a fragment")
    try:
        parsed = urlparse(value)
        port = parsed.port
    except ValueError as error:
        raise ConfigurationError(f"HORO_OTEL_ENDPOINT is invalid: {error}") from error

    _validate_url_structure(parsed, "HORO_OTEL_ENDPOINT")
    if parsed.hostname is None or port is None:
        raise ConfigurationError("HORO_OTEL_ENDPOINT must include a host and explicit port")
    if parsed.scheme == "http" and parsed.hostname not in {"127.0.0.1", "localhost", "::1"}:
        raise ConfigurationError("plaintext HORO_OTEL_ENDPOINT is allowed only for localhost")

    return Endpoint(url=value.rstrip("/"), host=parsed.hostname, port=port)


def load_settings(path: Path = DEFAULT_ENV_FILE, environ: Mapping[str, str] | None = None) -> DeveloperSettings:
    """Resolve defaults, local configuration, then process-environment overrides."""
    process_environment = os.environ if environ is None else environ
    resolved = {
        "HORO_DEV_OTEL_EXPORT": "OFF",
        "HORO_OTEL_ENDPOINT": DEFAULT_OTLP_ENDPOINT,
        **parse_dotenv(path),
        **process_environment,
    }
    export_enabled = parse_enabled(resolved["HORO_DEV_OTEL_EXPORT"])
    return DeveloperSettings(
        otel_export_enabled=export_enabled,
        endpoint=validate_endpoint(resolved["HORO_OTEL_ENDPOINT"] if export_enabled else DEFAULT_OTLP_ENDPOINT),
    )


def collector_is_reachable(endpoint: Endpoint, timeout_seconds: float = 0.5) -> bool:
    """Check only whether a TCP listener is reachable at the configured address."""
    try:
        with socket.create_connection((endpoint.host, endpoint.port), timeout=timeout_seconds):
            return True
    except OSError:
        return False


def _compiler_cache_arguments(
    compiler_launcher: str | None,
    msvc_debug_information_format: str | None,
) -> list[str]:
    """Configure both language launchers and cache-compatible MSVC debug output."""
    arguments = []
    if compiler_launcher:
        arguments.extend([
            f"-DCMAKE_C_COMPILER_LAUNCHER={compiler_launcher}",
            f"-DCMAKE_CXX_COMPILER_LAUNCHER={compiler_launcher}",
        ])
    if msvc_debug_information_format:
        arguments.append(f"-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT={msvc_debug_information_format}")
    return arguments


def configure_command(
    settings: DeveloperSettings | None = None,
    build_directory: Path = DEFAULT_BUILD_DIRECTORY,
    build_type: str = "Debug",
    testing: bool = False,
    editor_gui: bool = True,
    render_opengl: bool = True,
    render_metal: bool | None = None,
    telemetry: bool = True,
    opentelemetry: bool | None = None,
    imgui_ui_tests: bool = False,
    compiler_launcher: str | None = None,
    msvc_debug_information_format: str | None = None,
    extra_cmake_args: Sequence[str] | None = None,
) -> list[str]:
    """Build the typed, parameterizable CMake configure command."""
    if render_metal is None:
        render_metal = sys.platform == "darwin" and editor_gui
    if opentelemetry is None:
        opentelemetry = settings.otel_export_enabled if settings else False

    command = [
        "cmake",
        "-S",
        str(REPOSITORY_ROOT),
        "-B",
        str(build_directory),
        "-G",
        "Ninja",
        f"-DCMAKE_BUILD_TYPE={build_type}",
        f"-DHORO_BUILD_EDITOR_GUI={'ON' if editor_gui else 'OFF'}",
        f"-DHORO_BUILD_RENDER_OPENGL={'ON' if render_opengl else 'OFF'}",
        f"-DHORO_BUILD_RENDER_METAL={'ON' if render_metal else 'OFF'}",
        f"-DBUILD_TESTING={'ON' if testing else 'OFF'}",
        f"-DHORO_ENABLE_TELEMETRY={'ON' if telemetry else 'OFF'}",
        f"-DHORO_ENABLE_OPENTELEMETRY={'ON' if opentelemetry else 'OFF'}",
        f"-DHORO_ENABLE_IMGUI_UI_TESTS={'ON' if imgui_ui_tests else 'OFF'}",
    ]
    command.extend(_compiler_cache_arguments(compiler_launcher, msvc_debug_information_format))
    if extra_cmake_args:
        command.extend(extra_cmake_args)
    return command


def build_command(
    build_directory: Path = DEFAULT_BUILD_DIRECTORY,
    target: str | None = "HoroEditor",
    parallel: bool = True,
    jobs: int | None = None,
) -> list[str]:
    """Build the project or a specific target incrementally."""
    command = ["cmake", "--build", str(build_directory)]
    if target:
        command.extend(["--target", target])
    if parallel:
        command.append("--parallel")
        if jobs is not None:
            command.append(str(jobs))
    return command


def editor_executable(build_directory: Path = DEFAULT_BUILD_DIRECTORY, platform_name: str = sys.platform) -> Path:
    """Return the directly executable editor path for the host platform."""
    if platform_name == "darwin":
        return build_directory / "apps" / "HoroEditor.app" / "Contents" / "MacOS" / "HoroEditor"
    if platform_name == "win32":
        return build_directory / "apps" / "HoroEditor.exe"
    return build_directory / "apps" / "HoroEditor"


def child_environment(settings: DeveloperSettings, environ: Mapping[str, str] | None = None) -> dict[str, str]:
    """Create the editor environment with an explicit export policy."""
    child = dict(os.environ if environ is None else environ)
    child.pop("HORO_OTEL_EXPORT_APPROVED", None)
    child.pop("HORO_OTEL_ENDPOINT", None)
    child.pop("HORO_DEV_OTEL_EXPORT", None)
    if settings.otel_export_enabled:
        child["HORO_OTEL_EXPORT_APPROVED"] = "1"
        child["HORO_OTEL_ENDPOINT"] = settings.endpoint.url
    return child


class _RunnerConsole:
    """Small append-only terminal display; child diagnostics keep their original streams."""

    def __init__(self) -> None:
        self.width = max(16, min(78, shutil.get_terminal_size((80, 24)).columns - 4))
        self.color = sys.stdout.isatty() and "NO_COLOR" not in os.environ and os.environ.get("TERM") != "dumb"

    def paint(self, text: str, style: str) -> str:
        return f"\033[{style}m{text}\033[0m" if self.color else text

    def banner(self) -> None:
        logo = (
            r" _   _  ___  ____   ___ ",
            r"| | | |/ _ \|  _ \ / _ \ ",
            r"| |_| | | | | |_) | | | |",
            r"|  _  | |_| |  _ <| |_| |",
            r"|_| |_|\___/|_| \_\\___/",
            "       E N G I N E",
        )
        print()
        for line in logo if self.width >= 28 else ("HORO ENGINE",):
            print("  " + self.paint(line, "1;36"))
        print("\n  " + self.paint("DEVELOPER RUNNER", "1"))

    def panel(self, title: str, rows: Sequence[tuple[str, str]]) -> None:
        content_width = self.width - 4
        label_width = 16
        heading = f" {title} "[:self.width - 2].ljust(self.width - 2, "-")
        print("  " + self.paint("+" + heading + "+", "2"))
        for label, value in rows:
            compact = content_width < 36
            lines = textwrap.wrap(f"{label}: {value}" if compact else value,
                                  width=content_width if compact else content_width - label_width - 1) or [""]
            for index, line in enumerate(lines):
                prefix = "" if compact else f"{label if index == 0 else '':<{label_width}} "
                style = "1;32" if value.startswith("ON") else "2" if value == "OFF" else "1"
                padding = " " * (content_width - len(prefix) - len(line))
                print("  " + self.paint("| ", "2") + self.paint(prefix, "2") + self.paint(line, style)
                      + padding + self.paint(" |", "2"))
        print("  " + self.paint("+" + "-" * (self.width - 2) + "+", "2"))

    def stage(self, number: int, title: str) -> None:
        print("\n  " + self.paint(f"[{number}/3]", "1;36") + " " + self.paint(title.upper(), "1") + "\n", flush=True)

    def run_step(self, number: int, title: str, command: Sequence[str], env: Mapping[str, str] | None) -> int:
        self.stage(number, title)
        started = time.monotonic()
        code = execute_subprocess(command, env=env)
        status = self.paint("[OK]", "1;32") if code == 0 else self.paint(f"[FAIL {code}]", "1;31")
        print(f"\n  {status} {title} / {time.monotonic() - started:.1f}s", flush=True)
        return code


def print_summary(settings: DeveloperSettings, collector_status: str, build_directory: Path = DEFAULT_BUILD_DIRECTORY,
                  *, live_ui: bool = False, theme_file: Path | None = None) -> _RunnerConsole:
    """Print the effective non-secret developer configuration."""
    try:
        displayed_build_directory = build_directory.relative_to(REPOSITORY_ROOT)
    except ValueError:
        displayed_build_directory = build_directory
    console = _RunnerConsole()
    console.banner()
    console.panel("SESSION", [
        ("Target", "HoroEditor"),
        ("Build type", "Debug"),
        ("Host", f"{platform.system()} / {platform.machine()}"),
        ("Build directory", str(displayed_build_directory)),
    ])
    theme_source = str(theme_file) if theme_file is not None else os.environ.get("HORO_THEME_OVERRIDE", "")
    live_rows = [("Live C++", "ON / RCC++" if live_ui else "OFF"),
                 ("Live theme", "ON / JSON" if theme_source else "OFF")]
    if theme_source:
        live_rows.append(("Theme file", theme_source))
    console.panel("LIVE DEVELOPMENT", live_rows)
    diagnostics = [("OpenTelemetry", "ON" if settings.otel_export_enabled else "OFF")]
    if settings.otel_export_enabled:
        diagnostics.extend([("OTLP endpoint", settings.endpoint.url), ("Collector", collector_status)])
    else:
        diagnostics.append(("Collector", "not checked"))
    console.panel("DIAGNOSTICS", diagnostics)
    sys.stdout.flush()
    return console


def execute_subprocess(command: Sequence[str], cwd: Path = REPOSITORY_ROOT, env: Mapping[str, str] | None = None) -> int:
    """Run a subprocess with clear error reporting and interrupt handling."""
    try:
        # Arguments are passed directly as an argv sequence; no command shell parses caller-provided values.
        return run(command, cwd=cwd, env=env)
    except FileNotFoundError as error:
        print(f"error: command not found: {error.filename or command[0]}", file=sys.stderr)
        return 127
    except CleanupError as error:
        record_cleanup_failure(REPOSITORY_ROOT)
        print(f"error: subprocess cleanup was not confirmed; further builds are blocked: {error}", file=sys.stderr)
        return 126
    except OSError as error:
        print(f"error: failed to execute '{command[0]}': {error}", file=sys.stderr)
        return 126
    except KeyboardInterrupt as error:
        if isinstance(error.__cause__, CleanupError):
            record_cleanup_failure(REPOSITORY_ROOT)
            print("error: interrupted cleanup was not confirmed; further builds are blocked", file=sys.stderr)
        return 130
    except SystemExit as error:
        if isinstance(error.__cause__, CleanupError):
            record_cleanup_failure(REPOSITORY_ROOT)
            print("error: exiting cleanup was not confirmed; further builds are blocked", file=sys.stderr)
        raise


def _cached_compiler_flags(build_directory: Path) -> dict[str, str]:
    """Read previously configured language flags before adding path normalization."""
    cache = build_directory / "CMakeCache.txt"
    if not cache.is_file():
        return {}
    values = {}
    for line in cache.read_text(encoding="utf-8").splitlines():
        match = re.match(r"(CMAKE_C(?:XX)?_FLAGS):[^=]+=(.*)", line)
        if match:
            values[match[1]] = match[2]
    return values


def _cache_configure_arguments(command: Sequence[str], build_directory: Path, source: Path) -> list[str]:
    """Preserve user flags while normalizing debug paths for cross-worktree reuse."""
    result = list(command)
    result.append("-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON")
    if sys.platform == "win32":
        return result
    values: dict[str, str] = {}
    values.update(_cached_compiler_flags(build_directory))
    for argument in command:
        match = re.match(r"-D(CMAKE_C(?:XX)?_FLAGS)(?::[^=]+)?=(.*)", argument)
        if match:
            values[match[1]] = match[2]
    for language, variable in (("C", "CFLAGS"), ("CXX", "CXXFLAGS")):
        key = f"CMAKE_{language}_FLAGS"
        flags = values.get(key, os.environ.get(variable, ""))
        for directory in (source.resolve(), build_directory.resolve()):
            mapping = f"-fdebug-prefix-map={directory}=."
            if mapping not in shlex.split(flags):
                # CMake forwards flag strings through its try-compile parser;
                # double quotes preserve a path with spaces at both boundaries.
                quoted = '"' + mapping.replace("\\", "\\\\").replace('"', '\\"').replace("$", "\\$").replace("`", "\\`") + '"'
                flags = f"{flags} {quoted}".strip()
        result.append(f"-D{key}={flags}")
    return result


def _uses_explicit_cache(command: Sequence[str], launcher: str | None, check_environment: bool = True) -> bool:
    """Keep explicit launcher, PCH and cache-environment choices authoritative."""
    return bool(launcher) or any(
        argument.startswith(("-DCMAKE_C_COMPILER_LAUNCHER", "-DCMAKE_CXX_COMPILER_LAUNCHER"))
        or (argument.startswith("-DHORO_ENGINE_ENABLE_PCH") and _cmake_enabled(argument.rsplit("=", 1)[-1]))
        or (argument.startswith("-DCMAKE_DISABLE_PRECOMPILE_HEADERS") and not _cmake_enabled(argument.rsplit("=", 1)[-1]))
        for argument in command
    ) or (check_environment and any(key.startswith(("CCACHE_", "SCCACHE_")) for key in os.environ))


def _cmake_enabled(value: str) -> bool:
    """Interpret explicit CMake false constants; other cache strings are true."""
    normalized = value.upper()
    return normalized not in ("", "0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND") and not normalized.endswith("-NOTFOUND")


def _configure_and_build(command: Sequence[str], build_directory: Path, target: str | None,
                         compiler_launcher: str | None = None, use_cache: bool = True,
                         jobs: int = 2, clean: bool = False, console: _RunnerConsole | None = None) -> int:
    """Lease a completed main checkpoint for the complete configure/build lifecycle."""
    try:
        with heavy_build_lock(REPOSITORY_ROOT):
            if clean and build_directory.exists():
                print(f"Cleaning build directory {build_directory}...", flush=True)
                shutil.rmtree(build_directory)
            automatic = use_cache and sys.platform != "win32" and not _uses_explicit_cache(command, compiler_launcher)
            context = automatic_cache(REPOSITORY_ROOT) if automatic else nullcontext(None)
            with context as binding:
                arguments, environment = list(command), None
                if binding:
                    print(f"Using main cache {binding.main_commit[:12]} ({binding.generation})", flush=True)
                    arguments.extend(_compiler_cache_arguments(binding.launcher, None))
                    arguments = _cache_configure_arguments(arguments, build_directory, REPOSITORY_ROOT)
                    environment = binding.env
                code = console.run_step(1, "Configure", arguments, environment) if console else execute_subprocess(arguments, env=environment)
                if code:
                    return code
                build = build_command(build_directory, target, jobs=jobs)
                return console.run_step(2, "Build", build, environment) if console else execute_subprocess(build, env=environment)
    except (CacheError, OSError, ValueError) as error:
        print(f"error: local build setup failed: {error}", file=sys.stderr)
        return 2


def run_editor(settings: DeveloperSettings, editor_arguments: Sequence[str], build_directory: Path = DEFAULT_BUILD_DIRECTORY,
               theme_file: Path | None = None, live_ui: bool = False) -> int:
    """Configure, build, and launch HoroEditor without invoking a shell."""
    if theme_file is not None:
        theme_file = theme_file.resolve()
        if not theme_file.is_file():
            print(f"error: theme file not found: {theme_file}", file=sys.stderr)
            return 2
    if live_ui and sys.platform != "win32":
        print("error: --live-ui currently requires native Windows x64 and MSVC", file=sys.stderr)
        return 2
    collector_status = "not checked"
    if settings.otel_export_enabled:
        collector_status = "reachable" if collector_is_reachable(settings.endpoint) else "unreachable"
    console = print_summary(settings, collector_status, build_directory, live_ui=live_ui, theme_file=theme_file)

    if settings.otel_export_enabled and collector_status == "unreachable":
        print(
            f"warning: OpenTelemetry endpoint {settings.endpoint.host}:{settings.endpoint.port} is not reachable.\n"
            "The editor will continue with local diagnostics only.",
            file=sys.stderr,
        )

    build_code = _configure_and_build(
        configure_command(settings, build_directory=build_directory,
                          extra_cmake_args=[f"-DHORO_EDITOR_LIVE_UI={'ON' if live_ui else 'OFF'}"]), build_directory, "HoroEditor", console=console
    )
    if build_code != 0:
        return build_code


    executable = editor_executable(build_directory)
    if not executable.is_file():
        print(f"error: HoroEditor executable was not produced at {executable}", file=sys.stderr)
        return 1

    console.stage(3, "Launch")
    if live_ui:
        print("  Live C++: save a registered GUI .cpp to recompile and reload.\n", flush=True)
    environment = child_environment(settings)
    if theme_file is not None:
        environment["HORO_THEME_OVERRIDE"] = str(theme_file)
    return execute_subprocess(
        [str(executable), *editor_arguments],
        cwd=REPOSITORY_ROOT,
        env=environment,
    )


def run_build(
    target: str | None = None,
    build_directory: Path = DEFAULT_BUILD_DIRECTORY,
    build_type: str = "Debug",
    testing: bool = True,
    imgui_ui_tests: bool = False,
    clean: bool = False,
    compiler_launcher: str | None = None,
    msvc_debug_information_format: str | None = None,
    extra_cmake_args: Sequence[str] | None = None,
    fetchcontent_base_dir: Path | None = None,
    use_cache: bool = True,
    jobs: int = 2,
) -> int:
    """Configure and build the repository or a specific target."""
    print(f"Configuring project in {build_directory}...", flush=True)
    cmake_arguments = list(extra_cmake_args or ())
    if fetchcontent_base_dir is not None:
        cmake_arguments.insert(0, f"-DFETCHCONTENT_BASE_DIR={fetchcontent_base_dir.resolve()}")
    cfg_cmd = configure_command(
        build_directory=build_directory,
        build_type=build_type,
        testing=testing,
        editor_gui=True,
        render_opengl=True,
        render_metal=(sys.platform == "darwin"),
        telemetry=True,
        opentelemetry=True,
        imgui_ui_tests=imgui_ui_tests,
        compiler_launcher=compiler_launcher,
        msvc_debug_information_format=msvc_debug_information_format,
        extra_cmake_args=cmake_arguments,
    )
    print(f"Building {'target ' + target if target else 'all targets'}...", flush=True)
    return _configure_and_build(cfg_cmd, build_directory, target, compiler_launcher, use_cache, jobs, clean)


@dataclass(frozen=True)
class TestBuildCacheOptions:
    """Local checkpoint policy and compilation concurrency for test builds."""

    use_cache: bool = True
    jobs: int = 2

def run_tests(
    regex: str | None = None,
    exclude: str | None = None,
    gui: bool = False,
    build_directory: Path = DEFAULT_CI_BUILD_DIRECTORY,
    build_type: str = "Debug",
    junit: Path | None = None,
    compiler_launcher: str | None = None,
    msvc_debug_information_format: str | None = None,
    extra_ctest_args: Sequence[str] | None = None,
    fetchcontent_base_dir: Path | None = None,
    skip_build: bool = False,
    cache_options: TestBuildCacheOptions = TestBuildCacheOptions(),
    extra_cmake_args: Sequence[str] | None = None,
) -> int:
    """Build unless explicitly skipped, then execute CTest with optional filters."""
    if skip_build:
        if not (build_directory / "CTestTestfile.cmake").is_file():
            print(f"error: --skip-build requires a configured test directory: {build_directory}", file=sys.stderr)
            return 2
    else:
        build_code = run_build(
            target=None,
            build_directory=build_directory,
            build_type=build_type,
            testing=True,
            imgui_ui_tests=gui,
            compiler_launcher=compiler_launcher,
            msvc_debug_information_format=msvc_debug_information_format,
            fetchcontent_base_dir=fetchcontent_base_dir,
            use_cache=cache_options.use_cache,
            jobs=cache_options.jobs,
            extra_cmake_args=extra_cmake_args,
        )
        if build_code != 0:
            return build_code

    ctest_command = ["ctest", "--test-dir", str(build_directory), "--output-on-failure"]
    if skip_build:
        ctest_command.append("--no-tests=error")
    if not gui:
        ctest_command.extend(["-LE", "gui"])
    if regex:
        ctest_command.extend(["-R", regex])
    if exclude:
        ctest_command.extend(["-E", exclude])
    if junit:
        junit.parent.mkdir(parents=True, exist_ok=True)
        ctest_command.extend(["--output-junit", str(junit)])
    if extra_ctest_args:
        ctest_command.extend(extra_ctest_args)

    print(f"Running CTest suite in {build_directory}...", flush=True)
    return execute_subprocess(ctest_command)


def run_check(
    regex: str | None = None,
    gui: bool = False,
    build_directory: Path = DEFAULT_CI_BUILD_DIRECTORY,
    junit: Path | None = None,
    compiler_launcher: str | None = None,
    msvc_debug_information_format: str | None = None,
    fetchcontent_base_dir: Path | None = None,
    use_cache: bool = True,
    jobs: int = 2,
    extra_cmake_args: Sequence[str] | None = None,
) -> int:
    """Run full CI-parity check: configure all components, compile all targets, run test suite."""
    print("==================================================", flush=True)
    print("  Horo Engine CI Parity Verification Pass", flush=True)
    print("==================================================", flush=True)
    return run_tests(
        regex=regex,
        gui=gui,
        build_directory=build_directory,
        junit=junit,
        compiler_launcher=compiler_launcher,
        msvc_debug_information_format=msvc_debug_information_format,
        fetchcontent_base_dir=fetchcontent_base_dir,
        cache_options=TestBuildCacheOptions(use_cache, jobs),
        extra_cmake_args=extra_cmake_args,
    )


def _is_formattable(path: Path) -> bool:
    """Return true if path is a repository C++ source and not third-party."""
    try:
        relative = path.relative_to(REPOSITORY_ROOT) if path.is_absolute() else path
    except ValueError:
        return False
    return relative.suffix in _CPP_EXTENSIONS and not any(part in _EXCLUDED_PATH_PARTS for part in relative.parts)


def _collect_format_candidates(files: Sequence[str] | None, staged: bool) -> list[Path]:
    """Resolve file paths to format based on staged flag or explicit arguments."""
    if files:
        return [p for f in files if (p := Path(f).resolve()).is_file() and p.suffix in _CPP_EXTENSIONS]

    if staged:
        proc = subprocess.run(
            ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
            cwd=REPOSITORY_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        candidates = [REPOSITORY_ROOT / line.strip() for line in proc.stdout.splitlines() if line.strip()]
    else:
        proc = subprocess.run(["git", "ls-files"], cwd=REPOSITORY_ROOT, capture_output=True, text=True, check=False)
        candidates = [REPOSITORY_ROOT / line.strip() for line in proc.stdout.splitlines() if line.strip()]

    return [p for p in candidates if _is_formattable(p) and p.is_file()]


def _find_clang_format() -> str | None:
    """Resolve clang-format binary, preferring native executable over batch/cmd wrappers on Windows."""
    if sys.platform == "win32":
        exe = shutil.which("clang-format.exe")
        if exe:
            return exe
    return shutil.which("clang-format")


def _apply_format_chunk(clang_format_bin: str, chunk: list[str], check: bool) -> int:
    """Apply or verify clang-format on one chunk of files. Returns 0 on success."""
    if check:
        result = execute_subprocess([clang_format_bin, "--dry-run", "--Werror", *chunk])
        if result != 0:
            print("Formatting violations detected in above files.", file=sys.stderr)
            return 1
        return 0
    return execute_subprocess([clang_format_bin, "-i", *chunk])


def _partially_staged_files(paths: Sequence[Path]) -> list[Path]:
    """Return staged targets whose working-tree contents differ from the index."""
    relative_paths = [os.path.relpath(path, REPOSITORY_ROOT) for path in paths]
    proc = subprocess.run(
        ["git", "diff", "--name-only", "--diff-filter=ACMR", "--", *relative_paths],
        cwd=REPOSITORY_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        detail = proc.stderr.strip() or "git diff failed"
        raise RuntimeError(f"Could not inspect unstaged changes: {detail}")
    return [REPOSITORY_ROOT / line for line in proc.stdout.splitlines() if line]


def _validate_staged_format(target_files: Sequence[Path]) -> bool:
    try:
        partially_staged = _partially_staged_files(target_files)
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        return False
    if partially_staged:
        print(
            "error: refusing to format partially staged files; stage or stash their unstaged changes first:",
            file=sys.stderr,
        )
        for path in partially_staged:
            print(f"  {os.path.relpath(path, REPOSITORY_ROOT)}", file=sys.stderr)
        return False
    return True


def run_format(files: Sequence[str] | None = None, staged: bool = False, check: bool = False) -> int:
    """Format C++ source files with clang-format, with support for staged commits and dry-run check."""
    clang_format_bin = _find_clang_format()
    if not clang_format_bin:
        print("error: clang-format not found in PATH", file=sys.stderr)
        return 127

    target_files = _collect_format_candidates(files, staged)
    if not target_files:
        print("No matching C++ files to format.")
        return 0

    if staged and not check and not _validate_staged_format(target_files):
        return 1

    file_paths = [str(f) for f in target_files]
    chunk_size = 50
    chunks = [file_paths[i:i + chunk_size] for i in range(0, len(file_paths), chunk_size)]

    for chunk in chunks:
        code = _apply_format_chunk(clang_format_bin, chunk, check)
        if code != 0:
            return code

    if staged and not check:
        for chunk in chunks:
            subprocess.run(["git", "add", *chunk], cwd=REPOSITORY_ROOT, check=False)  # noqa: S603

    if check:
        print(f"All {len(target_files)} file(s) correctly formatted.")
    else:
        print(f"Successfully formatted {len(target_files)} file(s).")
    return 0


def _check_tool_version(
    report: DoctorReport,
    category: str,
    name: str,
    binary_candidates: Sequence[str | None],
    version_flag: str = "--version",
    missing_status: CheckStatus = CheckStatus.ERROR,
    missing_message: str | None = None,
) -> str | None:
    """Find a candidate binary, execute its version flag, and record diagnostic output."""
    found_bin: str | None = None
    for cand in binary_candidates:
        if cand and shutil.which(cand):
            found_bin = cand
            break

    if not found_bin:
        msg = missing_message or f"{name} not found in PATH"
        report.add(category, name, missing_status, msg)
        return None

    try:
        kwargs = {}
        if sys.platform == "win32" and found_bin.lower().endswith((".cmd", ".bat")):
            kwargs["shell"] = True
        proc = subprocess.run([found_bin, version_flag], capture_output=True, text=True, check=False, **kwargs)
        first_line = proc.stdout.splitlines()[0] if proc.stdout else "available"
        report.add(category, name, CheckStatus.OK, first_line, details=f"binary: {found_bin}")
        return found_bin
    except OSError as error:
        report.add(category, name, CheckStatus.ERROR, f"Failed to execute {name}: {error}")
        return None


def check_compiler(report: DoctorReport) -> None:
    """Verify C++ compiler availability and C++20 support."""
    cxx_env = os.environ.get("CXX")
    candidates = [cxx_env] if cxx_env else ["clang++", "g++", "cl.exe", "c++"]
    _check_tool_version(
        report,
        "Toolchain",
        _TOOLCHAIN_COMPILER_NAME,
        candidates,
        missing_message="No C++ compiler found in PATH or $CXX",
    )


def check_cmake(report: DoctorReport) -> None:
    """Verify CMake and Ninja build generator availability."""
    _check_tool_version(report, "Toolchain", _TOOLCHAIN_CMAKE_NAME, ["cmake"])
    _check_tool_version(
        report,
        "Toolchain",
        _TOOLCHAIN_NINJA_NAME,
        ["ninja"],
        missing_status=CheckStatus.WARN,
        missing_message="ninja not found; CMake may fall back to make/msbuild",
    )


def check_format_tool(report: DoctorReport) -> None:
    """Check if clang-format is available for code formatting."""
    _check_tool_version(
        report,
        "Toolchain",
        _TOOLCHAIN_CLANG_FORMAT_NAME,
        ["clang-format.exe", "clang-format"],
        missing_status=CheckStatus.WARN,
        missing_message="clang-format not found; pre-commit format check will be skipped",
    )


def check_ccache(report: DoctorReport) -> None:
    """Check if ccache is available for build acceleration."""
    _check_tool_version(
        report,
        "Toolchain",
        _TOOLCHAIN_CCACHE_NAME,
        ["ccache"],
        missing_status=CheckStatus.WARN,
        missing_message="ccache not found; installing ccache will significantly speed up rebuilds",
    )


def check_python_environment(report: DoctorReport) -> None:
    """Check Python version and test runners (pytest / .venv)."""
    py_ver = f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}"
    report.add("Python", "Interpreter", CheckStatus.OK, f"Python {py_ver} ({sys.executable})")

    venv_path = REPOSITORY_ROOT / ".venv"
    if venv_path.is_dir():
        report.add("Python", "Virtual Environment", CheckStatus.OK, f".venv present at {venv_path}")
    else:
        report.add("Python", "Virtual Environment", CheckStatus.INFO, ".venv not found in repository root")

    pytest_found = False
    try:
        import pytest  # noqa: F401
        report.add("Python", _PYTHON_PYTEST_NAME, CheckStatus.OK, f"pytest {pytest.__version__} installed")
        pytest_found = True
    except ImportError:
        pass

    if not pytest_found and venv_path.is_dir():
        venv_pytest = venv_path / "bin" / "pytest"
        if venv_pytest.is_file():
            report.add("Python", _PYTHON_PYTEST_NAME, CheckStatus.OK, f"pytest available via .venv ({venv_pytest})")
            pytest_found = True

    if not pytest_found:
        report.add("Python", _PYTHON_PYTEST_NAME, CheckStatus.WARN, "pytest not found; python test suite will be skipped in CTest")


def _check_darwin_graphics(report: DoctorReport) -> None:
    """Check macOS Metal, OpenGL, and window server status."""
    sdk_path = "/System/Library/Frameworks"
    metal_present = Path(f"{sdk_path}/Metal.framework").exists() or Path("/Applications/Xcode.app").exists()
    if metal_present:
        report.add("Graphics", "Metal API", CheckStatus.OK, "Apple Metal framework available")
    else:
        report.add("Graphics", "Metal API", CheckStatus.WARN, "Metal framework not found in standard system path")

    opengl_present = Path(f"{sdk_path}/OpenGL.framework").exists()
    if opengl_present:
        report.add("Graphics", "OpenGL API", CheckStatus.OK, "Apple OpenGL framework available")
    else:
        report.add("Graphics", "OpenGL API", CheckStatus.INFO, "OpenGL framework legacy path")

    has_pgrep = shutil.which("pgrep")
    window_server_active = False
    if has_pgrep:
        ws_proc = subprocess.run(["pgrep", "-x", "WindowServer"], capture_output=True, check=False)
        window_server_active = (ws_proc.returncode == 0)

    is_remote_ssh = "SSH_CLIENT" in os.environ or "SSH_CONNECTION" in os.environ
    if window_server_active and not is_remote_ssh:
        report.add("Graphics", _GRAPHICS_DISPLAY_NAME, CheckStatus.OK, "Interactive WindowServer session active")
    else:
        report.add("Graphics", _GRAPHICS_DISPLAY_NAME, CheckStatus.INFO, "Headless or remote session")


def _check_linux_graphics(report: DoctorReport) -> None:
    """Check Linux display and graphics environment."""
    display = os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")
    if display:
        report.add("Graphics", _GRAPHICS_DISPLAY_NAME, CheckStatus.OK, f"Display server active ({display})")
    else:
        report.add("Graphics", _GRAPHICS_DISPLAY_NAME, CheckStatus.INFO, "Headless display environment (DISPLAY not set)")


def check_platform_and_graphics(report: DoctorReport) -> None:
    """Verify host graphics and display environment capabilities."""
    current_os = platform.system()
    arch = platform.machine()
    report.add("Platform", "Host System", CheckStatus.OK, f"{current_os} {platform.release()} ({arch})")

    if current_os == "Darwin":
        _check_darwin_graphics(report)
    elif current_os == "Linux":
        _check_linux_graphics(report)


def check_observability_and_services(report: DoctorReport, settings: DeveloperSettings | None) -> None:
    """Check validity of developer settings and service reachability."""
    env_file = DEFAULT_ENV_FILE
    if env_file.exists():
        try:
            parse_dotenv(env_file)
            report.add("Configuration", DEFAULT_ENV_FILE_NAME, CheckStatus.OK, "Valid configuration file parsed")
        except ConfigurationError as error:
            report.add("Configuration", DEFAULT_ENV_FILE_NAME, CheckStatus.ERROR, f"Syntax/validation error: {error}")
    else:
        report.add("Configuration", DEFAULT_ENV_FILE_NAME, CheckStatus.INFO, f"No local {DEFAULT_ENV_FILE_NAME} file (using defaults)")

    if settings is None:
        report.add("Observability", _OBSERVABILITY_OTLP_NAME, CheckStatus.WARN, "Settings could not be loaded due to configuration error")
        return

    if settings.otel_export_enabled:
        reachable = collector_is_reachable(settings.endpoint)
        if reachable:
            report.add("Observability", _OBSERVABILITY_OTLP_NAME, CheckStatus.OK, f"Reachable at {settings.endpoint.url}")
        else:
            report.add("Observability", _OBSERVABILITY_OTLP_NAME, CheckStatus.WARN, f"Unreachable at {settings.endpoint.url} (fallback to local logs)")
    else:
        report.add("Observability", _OBSERVABILITY_OTLP_NAME, CheckStatus.INFO, "OpenTelemetry export disabled (HORO_DEV_OTEL_EXPORT=OFF)")


def _find_sonar_scanner() -> Path | None:
    """Locate the SonarScanner executable under tools/sonar-scanner-*/bin/ or in system PATH."""
    tools_dir = REPOSITORY_ROOT / "tools"
    if tools_dir.is_dir():
        for entry in sorted(tools_dir.iterdir(), reverse=True):
            if not entry.is_dir() or not entry.name.startswith("sonar-scanner"):
                continue
            bin_dir = entry / "bin"
            for candidate in ("sonar-scanner.bat", "sonar-scanner.cmd", "sonar-scanner"):
                exe = bin_dir / candidate
                if exe.is_file():
                    return exe
    if system_scanner := shutil.which("sonar-scanner"):
        return Path(system_scanner)
    return None


def _persist_sonar_token(token: str) -> None:
    """Append or update the sonar token in the repository-owned local environment file."""
    key = _SONAR_TOKEN_ENV_KEY
    if DEFAULT_ENV_FILE.exists():
        lines = DEFAULT_ENV_FILE.read_text(encoding="utf-8").splitlines(keepends=True)
        new_lines: list[str] = []
        replaced = False
        for line in lines:
            if line.startswith((f"{key}=", f"{key} =")):  # noqa: S8513
                new_lines.append(f"{key}={token}\n")
                replaced = True
            else:
                new_lines.append(line)
        if not replaced:
            if new_lines and not new_lines[-1].endswith("\n"):
                new_lines.append("\n")
            new_lines.append(f"{key}={token}\n")
        DEFAULT_ENV_FILE.write_text("".join(new_lines), encoding="utf-8")  # NOSONAR(S2083, S8707)
    else:
        DEFAULT_ENV_FILE.write_text(f"{key}={token}\n", encoding="utf-8")  # NOSONAR(S8707)


def _resolve_sonar_token(cli_token: str | None, env_file: Path = DEFAULT_ENV_FILE) -> str | None:
    """Resolve the SonarCloud token using the priority chain: CLI arg → env/dotenv → interactive prompt.

    When the token is obtained interactively, the user is offered the option to
    persist it to the local .env file so it is available in future runs.
    """
    # 1. Explicit CLI argument takes priority.
    if cli_token:
        return cli_token.strip()

    # 2. Environment variable / .env.local file.
    dotenv = parse_dotenv(env_file)
    from_env = os.environ.get(_SONAR_TOKEN_ENV_KEY) or dotenv.get(_SONAR_TOKEN_ENV_KEY)
    if from_env and from_env.strip():
        return from_env.strip()

    # 3. Interactive prompt — only if stdin is a TTY.
    if not sys.stdin.isatty():
        print(
            f"error: SonarCloud token not found. Set {_SONAR_TOKEN_ENV_KEY} in the environment, "
            f"{DEFAULT_ENV_FILE_NAME}, or pass --token.",
            file=sys.stderr,
        )
        return None

    print(f"SonarCloud token not found in environment or {DEFAULT_ENV_FILE_NAME}.")
    print(f"Set {_SONAR_TOKEN_ENV_KEY} in {DEFAULT_ENV_FILE_NAME} to skip this prompt in future runs.")
    try:
        token = input("Enter SonarCloud token: ").strip()
    except (EOFError, KeyboardInterrupt):
        print("", file=sys.stderr)
        return None

    if not token:
        print("error: empty token provided.", file=sys.stderr)
        return None

    # Offer to persist.
    try:
        answer = input(f"Save token to {DEFAULT_ENV_FILE_NAME} for future runs? [y/N] ").strip().lower()
    except (EOFError, KeyboardInterrupt):
        answer = ""
    if answer in {"y", "yes"}:
        try:
            _persist_sonar_token(token)
            print(f"Token saved to {DEFAULT_ENV_FILE_NAME}.")
        except OSError as error:
            print(f"warning: could not persist token: {error}", file=sys.stderr)

    return token


def run_sonar(
    token: str | None = None,
    pr_key: str | None = None,
    compile_commands: Path | None = None,
    env_file: Path = DEFAULT_ENV_FILE,
    extra_sonar_args: Sequence[str] | None = None,
) -> int:
    """Run the local SonarScanner against the current working tree and upload results to SonarCloud."""
    # Resolve token.
    resolved_token = _resolve_sonar_token(token, env_file)
    if not resolved_token:
        return 2

    # Locate scanner executable.
    scanner = _find_sonar_scanner()
    if scanner is None:
        print(
            "error: SonarScanner not found under tools/sonar-scanner-*/bin/.\n"
            "       Download from https://docs.sonarsource.com/sonarcloud/advanced-setup/ci-based-analysis/sonarscanner-cli/",
            file=sys.stderr,
        )
        return 127

    # Resolve compile-commands path.
    cc_path = compile_commands if compile_commands is not None else REPOSITORY_ROOT / _SONAR_DEFAULT_COMPILE_COMMANDS
    if not cc_path.is_file():
        print(
            f"error: compile_commands.json not found at {cc_path}.\n"
            "       Run 'python scripts/dev.py test' first to build the CI configuration.",
            file=sys.stderr,
        )
        return 1

    command: list[str] = [
        str(scanner),
        f"-Dsonar.token={resolved_token}",
        f"-Dsonar.cfamily.compile-commands={cc_path}",
    ]
    if pr_key:
        command.append(f"-Dsonar.pullrequest.key={pr_key}")
    if extra_sonar_args:
        command.extend(extra_sonar_args)

    print("Running SonarScanner...", flush=True)
    if pr_key:
        print(f"  Pull request: {pr_key}", flush=True)
    print(f"  Compile commands: {cc_path}", flush=True)
    print(f"  Scanner: {scanner}", flush=True)
    print()

    return execute_subprocess(command)


def run_doctor(settings: DeveloperSettings | None = None) -> DoctorReport:
    """Execute all diagnostic checks and assemble the report."""
    if settings is None:
        try:
            settings = load_settings()
        except (ConfigurationError, OSError):
            settings = None

    report = DoctorReport()
    check_compiler(report)
    check_cmake(report)
    check_format_tool(report)
    check_ccache(report)
    check_python_environment(report)
    check_platform_and_graphics(report)
    check_observability_and_services(report, settings)
    return report


_EPILOG_EXAMPLES = """examples:
  python3 scripts/dev.py doctor             Diagnose development toolchain and services
  python3 scripts/dev.py test               Run unit and integration test suite
  python3 scripts/dev.py test Scene         Run tests matching 'Scene'
  python3 scripts/dev.py check              Run full CI-parity verification pass
  python3 scripts/dev.py format --staged    Format git-staged C++ source files
  python3 scripts/dev.py build HoroEditor   Build specific target
  python3 scripts/dev.py sonar              Run local SonarScanner and upload to SonarCloud
  python3 scripts/dev.py sonar --pr 47      Run scan against pull request #47
  python3 scripts/dev.py run editor         Configure, build, and launch HoroEditor
  python3 scripts/dev.py help <command>     Show detailed options for a subcommand
"""


def _positive_jobs(value: str) -> int:
    """Reject an unbounded or invalid compile parallelism request."""
    jobs = int(value)
    if jobs < 1:
        raise argparse.ArgumentTypeError("jobs must be at least 1")
    return jobs


def run_cache_prepare(parsed: argparse.Namespace) -> int:
    """Populate a new checkpoint from an isolated, pinned origin/main checkout."""
    if sys.platform == "win32":
        print("error: managed main checkpoints currently require macOS or Linux; use an explicit Windows compiler launcher", file=sys.stderr)
        return 2
    if any(not argument.startswith("-D") for argument in parsed.cmake_arg):
        print("error: cache prepare accepts only -DNAME=value overrides; source and build directories are managed", file=sys.stderr)
        return 2
    if _uses_explicit_cache(parsed.cmake_arg, None, check_environment=False):
        print("error: cache prepare cannot override its managed launcher or enable PCH", file=sys.stderr)
        return 2
    if not parsed.offline:
        code = execute_subprocess(["git", "-C", str(REPOSITORY_ROOT), "fetch", "origin", "main"])
        if code:
            return code

    def build_main(source: Path, directory: Path, binding: CacheBinding) -> int:
        arguments = configure_command(
            build_directory=directory, build_type=parsed.type,
            testing=not parsed.no_testing, compiler_launcher=binding.launcher,
            telemetry=True, opentelemetry=True,
            extra_cmake_args=parsed.cmake_arg,
        )
        arguments[2] = str(source)
        if parsed.fetchcontent_base_dir:
            arguments.append(f"-DFETCHCONTENT_BASE_DIR={parsed.fetchcontent_base_dir.resolve()}")
        arguments = _cache_configure_arguments(arguments, directory, source)
        with heavy_build_lock(REPOSITORY_ROOT):
            code = execute_subprocess(arguments, cwd=source, env=binding.env)
            if code:
                return code
            return execute_subprocess(build_command(directory, parsed.target, jobs=parsed.jobs), cwd=source, env=binding.env)

    try:
        code = prepare_cache(REPOSITORY_ROOT, build_main, max_size=parsed.max_size)
        if code == 0:
            print(json.dumps(status(REPOSITORY_ROOT), indent=2))
        return code
    except (CacheError, OSError, ValueError) as error:
        print(f"error: cache preparation failed: {error}", file=sys.stderr)
        return 2


def _add_compiler_cache_arguments(parser: argparse.ArgumentParser) -> None:
    """Add dependency-directory and compiler-cache options shared by build commands."""
    parser.add_argument(
        "--fetchcontent-base-dir",
        type=Path,
        default=None,
        help="directory for CMake FetchContent dependencies",
    )
    parser.add_argument(
        "--compiler-launcher",
        default=None,
        help="CMake compiler launcher executable (for example sccache)",
    )
    parser.add_argument(
        "--msvc-debug-information-format",
        choices=("Embedded", "ProgramDatabase", "EditAndContinue"),
        default=None,
        help="select the MSVC debug information representation",
    )
    parser.add_argument("-j", "--jobs", type=_positive_jobs, default=2, help="compiler jobs (default: 2)")
    parser.add_argument("--no-cache", action="store_true", help="disable automatic main checkpoint discovery")
    parser.add_argument("--cmake-arg", action="append", default=[], help="extra configure argument; use --cmake-arg=-DNAME=value")


def create_argument_parser() -> argparse.ArgumentParser:
    """Create the developer command-line interface argument parser."""
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog=_EPILOG_EXAMPLES,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    commands = parser.add_subparsers(dest="command", required=False)

    cache = commands.add_parser("cache", help="prepare or inspect a main-derived compiler checkpoint")
    cache_commands = cache.add_subparsers(dest="cache_command", required=True)
    cache_commands.add_parser("status", help="show the current completed main checkpoint")
    prepare = cache_commands.add_parser("prepare", help="build pinned origin/main and atomically publish a checkpoint")
    prepare.add_argument("--target", default=None, help="optional target to warm (default: all targets)")
    prepare.add_argument("--type", default="Debug", choices=("Debug", "Release", "RelWithDebInfo"))
    prepare.add_argument("--max-size", default="4G", help="compiler-cache budget per generation (default: 4G)")
    prepare.add_argument("--offline", action="store_true", help="use the existing origin/main ref without fetching")
    prepare.add_argument("--no-testing", action="store_true")
    prepare.add_argument("--fetchcontent-base-dir", type=Path, default=None)
    prepare.add_argument("--cmake-arg", action="append", default=[], help="extra configure argument; use --cmake-arg=-DNAME=value")
    prepare.add_argument("-j", "--jobs", type=_positive_jobs, default=2)

    for name in TOOL_COMMANDS:
        commands.add_parser(name, add_help=False, help="delivery and CI operation (use command --help)")
    commands.add_parser("hook", add_help=False, help="run optional pre-commit or pre-push checks")

    # run command
    run = commands.add_parser("run", help="configure, build, and run a supported target")
    run.add_argument("target_or_args", nargs="*", default=[], help="target (default 'editor') or arguments forwarded")
    run.add_argument("--theme-file", type=Path, default=None, help="load and live-reload an external theme JSON in Debug")
    run.add_argument("--live-ui", action="store_true", help="enable the RCC++ live GUI presentation (Windows x64 Debug)")

    # doctor command
    doc = commands.add_parser("doctor", help="diagnose toolchain, graphics, and environment health")
    doc.add_argument("--json", action="store_true", help="output report as structured JSON")
    doc.add_argument("--no-color", action="store_true", help="disable ANSI color formatting")

    # build command
    bld = commands.add_parser("build", help="configure and build all targets or a specific target")
    bld.add_argument("target", nargs="?", default=None, help="specific target name to build (e.g. HoroEditor, HoroEditorUiAutomationTests)")
    bld.add_argument("-B", "--dir", type=Path, default=DEFAULT_BUILD_DIRECTORY, help="build output directory")
    bld.add_argument("--type", default="Debug", choices=("Debug", "Release", "RelWithDebInfo"), help="CMake build type")
    bld.add_argument("--clean", action="store_true", help="clean build directory before configuring")
    bld.add_argument("--no-testing", action="store_true", help="disable building tests")
    _add_compiler_cache_arguments(bld)

    # test command
    tst = commands.add_parser("test", help="configure with tests, compile, and run CTest suite")
    tst.add_argument("regex", nargs="?", default=None, help="test name filter regex (passed to ctest -R)")
    tst.add_argument("-R", "--regex-flag", dest="regex_opt", default=None, help="explicit regex filter option")
    tst.add_argument("-E", "--exclude", default=None, help="exclude tests matching regex")
    tst.add_argument("--gui", action="store_true", help="include interactive GUI test engine scenarios")
    tst.add_argument("-B", "--dir", type=Path, default=DEFAULT_CI_BUILD_DIRECTORY, help="test build directory")
    tst.add_argument("--type", default="Debug", choices=("Debug", "Release", "RelWithDebInfo"), help="CMake build type")
    tst.add_argument("--junit", type=Path, default=None, help="write CTest results to JUnit XML file")
    tst.add_argument("--skip-build", action="store_true", help="run CTest against an already built test directory")
    _add_compiler_cache_arguments(tst)

    # check command (CI Parity)
    chk = commands.add_parser("check", help="run full CI-parity build and comprehensive test pass")
    chk.add_argument("-R", "--regex", default=None, help="filter tests during check")
    chk.add_argument("--gui", action="store_true", help="include interactive GUI scenarios")
    chk.add_argument("-B", "--dir", type=Path, default=DEFAULT_CI_BUILD_DIRECTORY, help="check build directory")
    chk.add_argument("--junit", type=Path, default=None, help="write CTest results to JUnit XML file")
    _add_compiler_cache_arguments(chk)

    # format command
    fmt = commands.add_parser("format", help="format C++ source files using clang-format")
    fmt.add_argument("files", nargs="*", default=None, help="specific files to format")
    fmt.add_argument("--staged", action="store_true", help="format only git-staged C++ files and re-add them")
    fmt.add_argument("--check", action="store_true", help="check formatting without modifying files")

    # help command
    hlp = commands.add_parser("help", help="show general help or help for a specific command")
    hlp.add_argument("topic", nargs="?", default=None, help="subcommand name to show detailed help for")

    # sonar command
    snr = commands.add_parser(
        "sonar",
        help="run local SonarScanner and upload analysis to SonarCloud",
        description=(
            "Run the bundled SonarScanner CLI against the repository and upload the analysis report to SonarCloud.\n\n"
            "Token resolution order:\n"
            "  1. --token argument\n"
            f"  2. {_SONAR_TOKEN_ENV_KEY} environment variable\n"
            f"  3. {_SONAR_TOKEN_ENV_KEY} entry in {DEFAULT_ENV_FILE_NAME}\n"
            "  4. Interactive prompt (TTY only) with optional save to .env.local"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    snr.add_argument(
        "--token",
        default=None,
        metavar="TOKEN",
        help=f"SonarCloud authentication token (overrides {_SONAR_TOKEN_ENV_KEY} env var / .env.local)",
    )
    snr.add_argument(
        "--pr",
        "--pullrequest",
        dest="pr_key",
        default=None,
        metavar="PR_NUMBER",
        help="pull request key to analyze (e.g. 47)",
    )
    snr.add_argument(
        "--compile-commands",
        type=Path,
        default=None,
        metavar="PATH",
        help=f"path to compile_commands.json (default: {_SONAR_DEFAULT_COMPILE_COMMANDS})",
    )

    return parser


def _handle_sonar(parsed: argparse.Namespace, extra_args: Sequence[str]) -> int:
    """Handle sonar subcommand."""
    return run_sonar(
        token=parsed.token,
        pr_key=parsed.pr_key,
        compile_commands=parsed.compile_commands,
        extra_sonar_args=list(extra_args) if extra_args else None,
    )


def _handle_run(parsed: argparse.Namespace, settings: DeveloperSettings, extra_args: Sequence[str] | None = None) -> int:
    """Handle run subcommand."""
    raw_args = list(parsed.target_or_args)
    if extra_args:
        raw_args.extend(extra_args)
    if raw_args and raw_args[0] == "editor":
        raw_args.pop(0)
    if raw_args and raw_args[0] == "--":
        raw_args.pop(0)
    try:
        return run_editor(settings, raw_args, theme_file=parsed.theme_file, live_ui=parsed.live_ui)
    except KeyboardInterrupt:
        return 130


def _handle_doctor(parsed: argparse.Namespace, settings: DeveloperSettings | None) -> int:
    """Handle doctor subcommand."""
    report = run_doctor(settings)
    if getattr(parsed, "json", False):
        print(json.dumps(report.to_dict(), indent=2))
    else:
        use_color = sys.stdout.isatty() and not getattr(parsed, "no_color", False)
        print(report.render(use_color=use_color))
    return 1 if report.has_errors else 0


def _handle_help(parser: argparse.ArgumentParser, topic: str | None) -> int:
    """Show top-level help or topic-specific subcommand help."""
    if not topic:
        parser.print_help()
        return 0

    # Note: accessing _actions is an internal argparse hook used to display specific subparser help
    subparser_action = next(
        (action for action in parser._actions if isinstance(action, argparse._SubParsersAction)),
        None,
    )
    if subparser_action and topic in subparser_action.choices:
        subparser_action.choices[topic].print_help()
        return 0

    print(f"error: unknown command {topic!r}. Run 'dev.py help' for available commands.", file=sys.stderr)
    return 2


def _configure_terminal_environment() -> None:
    """Enable ANSI/UTF-8 mode on Windows consoles and configure robust encoding fallbacks."""
    if sys.platform == "win32":
        try:
            os.system("")  # Enable VT100 ANSI processing on Windows 10+
        except OSError:
            pass
    if sys.version_info >= (3, 7):
        try:
            if hasattr(sys.stdout, "reconfigure"):
                sys.stdout.reconfigure(errors="replace")
            if hasattr(sys.stderr, "reconfigure"):
                sys.stderr.reconfigure(errors="replace")
        except (AttributeError, OSError, ValueError):
            pass


def _require_no_unparsed(parser: argparse.ArgumentParser, unparsed: Sequence[str]) -> None:
    """Ensure no extra positional or unknown option arguments were provided."""
    if unparsed:
        parser.error(f"unrecognized arguments: {' '.join(unparsed)}")


def _handle_cache(parsed: argparse.Namespace) -> int:
    """Prepare a checkpoint or report its current availability."""
    if parsed.cache_command == "prepare":
        return run_cache_prepare(parsed)
    try:
        print(json.dumps(status(REPOSITORY_ROOT), indent=2))
        return 0
    except CacheError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


def _dispatch_command(parsed: argparse.Namespace, unparsed: Sequence[str], parser: argparse.ArgumentParser) -> int:
    """Dispatch parsed subcommand to its respective handler."""
    if not parsed.command or parsed.command == "help":
        return _handle_help(parser, getattr(parsed, "topic", None))

    if parsed.command == "cache":
        _require_no_unparsed(parser, unparsed)
        return _handle_cache(parsed)

    if parsed.command == "doctor":
        try:
            doc_settings = load_settings()
        except (ConfigurationError, OSError):
            doc_settings = None
        return _handle_doctor(parsed, doc_settings)

    if parsed.command == "format":
        _require_no_unparsed(parser, unparsed)
        return run_format(files=parsed.files, staged=parsed.staged, check=parsed.check)

    if parsed.command == "sonar":
        return _handle_sonar(parsed, unparsed)

    if parsed.command == "build":
        _require_no_unparsed(parser, unparsed)
        return run_build(
            target=parsed.target,
            build_directory=parsed.dir,
            build_type=parsed.type,
            testing=not parsed.no_testing,
            clean=parsed.clean,
            compiler_launcher=parsed.compiler_launcher,
            msvc_debug_information_format=parsed.msvc_debug_information_format,
            fetchcontent_base_dir=parsed.fetchcontent_base_dir,
            use_cache=not parsed.no_cache,
            jobs=parsed.jobs,
            extra_cmake_args=parsed.cmake_arg,
        )

    if parsed.command == "test":
        _require_no_unparsed(parser, unparsed)
        active_regex = parsed.regex_opt or parsed.regex
        return run_tests(
            regex=active_regex,
            exclude=parsed.exclude,
            gui=parsed.gui,
            build_directory=parsed.dir,
            build_type=parsed.type,
            junit=parsed.junit,
            compiler_launcher=parsed.compiler_launcher,
            msvc_debug_information_format=parsed.msvc_debug_information_format,
            fetchcontent_base_dir=parsed.fetchcontent_base_dir,
            skip_build=parsed.skip_build,
            cache_options=TestBuildCacheOptions(not parsed.no_cache, parsed.jobs),
            extra_cmake_args=parsed.cmake_arg,
        )

    if parsed.command == "check":
        _require_no_unparsed(parser, unparsed)
        return run_check(
            regex=parsed.regex,
            gui=parsed.gui,
            build_directory=parsed.dir,
            junit=parsed.junit,
            compiler_launcher=parsed.compiler_launcher,
            msvc_debug_information_format=parsed.msvc_debug_information_format,
            fetchcontent_base_dir=parsed.fetchcontent_base_dir,
            use_cache=not parsed.no_cache,
            jobs=parsed.jobs,
            extra_cmake_args=parsed.cmake_arg,
        )

    # run command
    try:
        settings = load_settings()
    except (ConfigurationError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    return _handle_run(parsed, settings, unparsed)


def developer_main(arguments: Sequence[str] | None = None) -> int:
    """Run the developer command-line interface."""
    _configure_terminal_environment()
    parser = create_argument_parser()
    parsed, unparsed = parser.parse_known_args(arguments)
    return _dispatch_command(parsed, unparsed, parser)


def run_hook(name: str) -> int:
    """Run optional hook checks; never install or edit the operator's Git hooks."""
    if name == "pre-commit":
        return main(["format", "--staged"])
    if name == "pre-push":
        return main(["check"])
    raise ValueError("Unknown Git hook")


def setup_main(argv=None) -> int:
    """Configure and smoke-build with an existing native toolchain; never install globally."""
    parser = argparse.ArgumentParser(description="Initialize a build with the installed native toolchain")
    parser.add_argument("-B", "--dir", type=Path, default=DEFAULT_BUILD_DIRECTORY)
    parser.add_argument("--target", default="HoroFoundation")
    parser.add_argument("-j", "--jobs", type=_positive_jobs, default=2)
    parser.add_argument("--no-cache", action="store_true")
    args = parser.parse_args(argv)
    return run_build(target=args.target, build_directory=args.dir, jobs=args.jobs, use_cache=not args.no_cache)


def open_pr_main(argv=None) -> int:
    """Create a PR only on explicit invocation, preserving the former shell contract."""
    parser = argparse.ArgumentParser(description="Create a pull request from a reviewed body file")
    parser.add_argument("title")
    parser.add_argument("body_file", type=Path)
    parser.add_argument("base")
    parser.add_argument("head")
    parser.add_argument("milestone")
    parser.add_argument("project", nargs="?")
    args = parser.parse_args(argv)
    if not args.body_file.is_file():
        parser.error("body_file must be an existing reviewed file")
    command = ["gh", "pr", "create", "--title", args.title, "--body-file", str(args.body_file),
               "--base", args.base, "--head", args.head, "--assignee", "@me", "--milestone", args.milestone]
    if args.project:
        command.extend(["--project", args.project])
    return execute_subprocess(command)


def select_manifest(package_paths: list[str]) -> Path:
    """Require one installed lavapipe manifest, without a GPU fallback."""
    candidates = sorted({Path(path) for path in package_paths
                         if Path(path).name.startswith("lvp_icd") and Path(path).suffix == ".json"
                         and Path(path).is_file()})
    if len(candidates) != 1:
        raise ValueError(f"Expected exactly one installed lavapipe ICD manifest, found {len(candidates)}")
    selected = candidates[0].resolve(strict=True)
    manifest = json.loads(selected.read_text(encoding="utf-8"))
    if not isinstance(manifest.get("ICD", {}).get("library_path"), str) or not manifest["ICD"]["library_path"]:
        raise ValueError("Installed lavapipe ICD manifest has no library path")
    return selected


def select_lavapipe_main(argv=None) -> int:
    """Print the Mesa package's actual lavapipe path for native qualification."""
    parser = argparse.ArgumentParser(description=select_lavapipe_main.__doc__)
    parser.parse_args(argv)
    inventory = subprocess.run(["dpkg-query", "-L", "mesa-vulkan-drivers"], check=True,
                               capture_output=True, text=True)
    print(select_manifest(inventory.stdout.splitlines()))
    return 0


def _gui_live_symbols(path: Path):
    """Read ordinary and bigobj COFF external symbols without executing dumpbin."""
    data = path.read_bytes()
    if data[:4] == b'\x00\x00\xff\xff':
        start, count = struct.unpack_from('<II', data, 48)
        size, section_format, storage = 20, '<i', 18
    else:
        start, count = struct.unpack_from('<II', data, 8)
        size, section_format, storage = 18, '<h', 16
    strings = start + count * size
    index = 0
    while index < count:
        offset = start + index * size
        section = struct.unpack_from(section_format, data, offset + 12)[0]
        symbol_type = struct.unpack_from('<H', data, offset + storage - 2)[0]
        if data[offset + storage] == 2:
            raw = data[offset:offset + 8]
            if raw[:4] == b'\x00' * 4:
                at = strings + struct.unpack_from('<I', raw, 4)[0]
                name = data[at:data.index(0, at)].decode('utf-8')
            else:
                name = raw.split(b'\x00')[0].decode('utf-8')
            yield name, symbol_type, section == 0
        index += 1 + data[offset + storage + 1]


def _generate_gui_live_bridge(database: Path, output: Path, image: str):
    entries = json.loads(database.read_text(encoding='utf-8'))
    gui = [entry for entry in entries if 'HoroGui.dir' in entry['command']]
    if not gui:
        raise ValueError('No HoroGui compile commands in the selected build')
    exports = set()
    for entry in gui:
        match = re.search(r'(?:^|\s)/Fo(?:"([^"]+)"|(\S+))', entry['command'])
        if not match:
            raise ValueError(f"Missing MSVC object path for {entry['file']}")
        obj = Path(entry['directory']) / (match[1] or match[2])
        for name, kind, undefined in _gui_live_symbols(obj):
            shared_function = undefined and ('@Horo@@' in name or name.startswith('?Im') or any(owner in name for owner in ('ImGui', 'ImFont', 'ImDraw')))
            if kind == 0x20 and (shared_function or name.startswith('??$HostAddress@')):
                exports.add(name)
    # ImGui API imports use the very same owner-thread context as the executable.
    exports.add('?GImGui@@3PEAUImGuiContext@@EA DATA')
    if not exports or len(exports) >= 65535:
        raise ValueError(f'Invalid GUI bridge export count: {len(exports)}')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text('EXPORTS\n' + ''.join(f'    {name}\n' for name in sorted(exports)), encoding='utf-8')
    definitions = re.findall(r'(?:^|\s)([-/]D(?:"[^"\n]+"|\S+))', gui[0]['command'])
    flags = [ '/std:c++20', '/utf-8', '/EHsc', '/D_DEBUG', '/DNOMINMAX', '/DHORO_GUI_RUNTIME_COMPILATION=1', '/DIMGUI_API=__declspec(dllimport)', *definitions]
    output.with_suffix('.rsp').write_text('\n'.join(flags) + '\n', encoding='utf-8')
    print(f'Generated {len(exports)} private GUI host imports for {image}')



def gui_live_bridge_main(argv=None) -> int:
    """Generate the private native GUI import bridge for a configured development host."""
    parser = argparse.ArgumentParser(description=gui_live_bridge_main.__doc__)
    parser.add_argument("--compile-commands", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--image", required=True)
    args = parser.parse_args(argv)
    _generate_gui_live_bridge(args.compile_commands, args.output, args.image)
    return 0


TOOL_COMMANDS = {"gui-live-bridge": gui_live_bridge_main, "select-lavapipe-icd": select_lavapipe_main, "setup": setup_main, "open-pr": open_pr_main, 'ci-cache-identity': ci_cache_main, 'release-notes': notes_main, 'verify-release-archive': archive_main, 'verify-release-notes': release_main, 'pr-annotations': pr_main, 'qualify-scene': scene_main, 'qualify-character': character_main, 'qualify-extension': extension_main, 'qualify-save': save_main}


def main(argv=None) -> int:
    """Dispatch developer and delivery operations without spawning another script."""
    argv = list(sys.argv[1:] if argv is None else argv)
    if len(argv) == 2 and argv[0] == "help" and argv[1] in TOOL_COMMANDS:
        return TOOL_COMMANDS[argv[1]](["--help"]) or 0
    if argv and argv[0] in TOOL_COMMANDS:
        try:
            return TOOL_COMMANDS[argv[0]](argv[1:]) or 0
        except (ValueError, OSError) as error:
            print(str(error), file=sys.stderr)
            return 1
    if argv and argv[0] == "hook":
        parser = argparse.ArgumentParser(description="Run optional Git hook checks")
        parser.add_argument("name", choices=("pre-commit", "pre-push"))
        return run_hook(parser.parse_args(argv[1:]).name)
    return developer_main(argv)


if __name__ == "__main__":
    raise SystemExit(main())
