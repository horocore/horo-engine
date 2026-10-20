"""Local quality analysis, owned IDE sessions, secrets, metrics and coverage."""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import cast
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen
import argparse
import base64
import binascii
import concurrent.futures
import contextlib
import csv
import fnmatch
import functools
import hashlib
import io
import json
import os
import re
import shutil
import signal
import sqlite3
import stat
import subprocess
import sys
import threading
import time
import uuid
import xml.etree.ElementTree as element_tree


# Bridge

#!/usr/bin/env python3
# Codacy profiles alternate between the mutually exclusive D212 and D213 layouts.
# noqa: D212,D213









# Process execution is restricted to resolved, fixed tool names below.
  # nosec B404










SUPPORTED_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".tpp"})
TRANSLATION_UNIT_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx"})
DEFAULT_BUILD_DIRECTORY = Path("build/sonar-local")
COMPILE_COMMANDS_FILENAME = "compile_commands.json"
DEFAULT_COMPILE_COMMANDS = DEFAULT_BUILD_DIRECTORY / COMPILE_COMMANDS_FILENAME
BRIDGE_PORTS = range(64120, 64131)
BRIDGE_STATUS_PATH = "/sonarlint/api/status"
BRIDGE_ANALYZE_PATH = "/sonarlint/api/analysis/files"
BRIDGE_PATHS = frozenset({BRIDGE_STATUS_PATH, BRIDGE_ANALYZE_PATH})
FIXED_TOOLS = frozenset({"cmake", "code", "git"})
GIT_REF_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9._/-]{0,254}")


class AnalysisError(RuntimeError):

    # Codacy profiles alternate between the mutually exclusive D203 and D211 layouts.
    # noqa: D203,D211
    """A prerequisite or the local IDE bridge prevented analysis."""


@dataclass(frozen=True)
class Selection:

    # Codacy profiles alternate between the mutually exclusive D203 and D211 layouts.
    # noqa: D203,D211
    """Files selected for one bridge request and paths intentionally omitted."""

    submitted: list[Path]
    skipped: list[dict[str, str]]
    source: str





def resolved_tool(name: str) -> str:
    """Resolve one fixed prerequisite without delegating lookup to a child process."""
    if name not in FIXED_TOOLS:
        raise AnalysisError(f"Unsupported prerequisite: {name}")
    executable = shutil.which(name)
    if executable is None:
        raise AnalysisError(f"{name} is required")
    return executable


def run_command(tool: str, arguments: Sequence[str], error_message: str) -> subprocess.CompletedProcess[str]:
    """Run a fixed prerequisite without contaminating JSON stdout."""
    if tool == "cmake":
        executable = resolved_tool("cmake")
        # Bandit cannot infer the validated absolute executable; argv[0] is literal and shell execution is disabled.
        result = subprocess.run(  # nosec B603, B607
            ["cmake", *arguments],
            executable=executable,
            check=False,
            capture_output=True,
            text=True,
            shell=False,
        )
    elif tool == "code":
        executable = resolved_tool("code")
        # Bandit cannot infer the validated absolute executable; argv[0] is literal and shell execution is disabled.
        result = subprocess.run(  # nosec B603, B607
            ["code", *arguments],
            executable=executable,
            check=False,
            capture_output=True,
            text=True,
            shell=False,
        )
    else:
        raise AnalysisError(f"Unsupported prerequisite: {tool}")
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise AnalysisError(f"{error_message}{': ' + detail if detail else ''}")
    return result


def run_git(root: Path, *arguments: str) -> bytes:
    """Run Git in *root* and return its NUL-safe stdout."""
    try:
        executable = resolved_tool("git")
        # Callers validate external refs and place them after Git's end-of-options marker.
        # Bandit cannot infer the validated absolute executable; argv[0] is literal and shell execution is disabled.
        return subprocess.run(  # nosec B603, B607
            ["git", *arguments],
            cwd=root.resolve(strict=True),
            executable=executable,
            check=True,
            capture_output=True,
            shell=False,
        ).stdout
    except subprocess.CalledProcessError as error:
        detail = error.stderr.decode("utf-8", errors="replace").strip()
        raise AnalysisError(detail or "git could not select files for analysis") from error


def repository_root() -> Path:
    """Return the root of the current Git worktree."""
    output = run_git(Path.cwd(), "rev-parse", "--show-toplevel")
    return Path(output.decode("utf-8").strip()).resolve()


def paths_from_nul_output(output: bytes) -> list[Path]:
    """Decode Git's NUL-delimited paths without losing spaces or Unicode."""
    return [Path(item.decode("utf-8", errors="surrogateescape")) for item in output.split(b"\0") if item]


def changed_paths(root: Path, base: str | None) -> tuple[list[Path], str]:
    """Return changed paths for a base ref or the current dirty worktree."""
    if base is not None:
        base = validated_git_ref(base)
        merge_base = run_git(root, "merge-base", "--", base, "HEAD").decode("utf-8").strip()
        tracked = paths_from_nul_output(run_git(root, "diff", "--name-only", "--diff-filter=ACMR", "-z", merge_base))
        untracked = paths_from_nul_output(run_git(root, "ls-files", "--others", "--exclude-standard", "-z"))
        return list(dict.fromkeys([*tracked, *untracked])), f"base:{base}"

    records = run_git(root, "status", "--porcelain=v1", "--untracked-files=all", "-z").split(b"\0")
    paths: list[Path] = []
    index = 0
    while index < len(records):
        record = records[index]
        index += 1
        if not record:
            continue
        if len(record) < 4:
            raise AnalysisError("Git returned an invalid porcelain status record")
        paths.append(Path(record[3:].decode("utf-8", errors="surrogateescape")))
        if record[0:1] in {b"R", b"C"} or record[1:2] in {b"R", b"C"}:
            index += 1
    return paths, "dirty"


def validated_git_ref(value: str) -> str:
    """Accept only an option-safe, bounded Git ref spelling."""
    if GIT_REF_PATTERN.fullmatch(value) is None or ".." in value or "//" in value or value.endswith(("/", ".")):
        raise AnalysisError("--base must be a conventional Git ref name")
    return value


def select_files(root: Path, requested: Sequence[Path], source: str) -> Selection:
    """Keep existing worktree C/C++ files and explain every skipped input."""
    submitted: list[Path] = []
    skipped: list[dict[str, str]] = []
    seen: set[Path] = set()
    for candidate in requested:
        path = (candidate if candidate.is_absolute() else root / candidate).resolve()
        try:
            path.relative_to(root)
        except ValueError:
            skipped.append({"path": str(candidate), "reason": "outside_worktree"})
            continue
        if not path.is_file():
            skipped.append({"path": str(candidate), "reason": "missing_or_not_regular_file"})
            continue
        if path.suffix.lower() not in SUPPORTED_SUFFIXES:
            skipped.append({"path": str(candidate), "reason": "unsupported_file_type"})
            continue
        if path not in seen:
            submitted.append(path)
            seen.add(path)
    return Selection(submitted, skipped, source)





def _load_compilation_database(compilation_database: Path) -> list[object]:
    """Read and validate the top-level shape of a compilation database."""
    try:
        entries = json.loads(compilation_database.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise AnalysisError(f"Cannot read compilation database: {compilation_database}") from error
    if not isinstance(entries, list):
        raise AnalysisError(f"Compilation database is not an array: {compilation_database}")
    return cast(list[object], entries)


def _compiled_sources(root: Path, compilation_database: Path, entries: Sequence[object]) -> set[Path]:
    """Collect the in-worktree source paths represented by a compilation database."""
    compiled_files: set[Path] = set()
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("file"), str):
            continue
        directory_value = entry.get("directory")
        directory = Path(directory_value) if isinstance(directory_value, str) else compilation_database.parent
        source = Path(entry["file"])
        absolute_source = Path(os.path.abspath(source if source.is_absolute() else directory / source))
        if absolute_source.is_relative_to(root):
            compiled_files.add(absolute_source)
    return compiled_files


def validate_compile_commands(root: Path, compilation_database: Path, files: Sequence[Path]) -> None:
    """Require a command for every submitted translation unit; headers use IDE context."""
    entries = _load_compilation_database(compilation_database)
    compiled_files = _compiled_sources(root, compilation_database, entries)
    missing = [
        str(path)
        for path in files
        if path.suffix.lower() in TRANSLATION_UNIT_SUFFIXES and path not in compiled_files
    ]
    if missing:
        raise AnalysisError("Compilation database has no command for submitted file(s): " + ", ".join(missing))


def bridge_url(port: int, path: str) -> str:
    """Build a loopback-only bridge URL after validating Sonar's port range."""
    if port not in BRIDGE_PORTS:
        raise AnalysisError("--port must be in SonarQube for IDE's 64120-64130 range")
    if path not in BRIDGE_PATHS:
        raise AnalysisError("Unsupported SonarQube for IDE bridge path")
    return f"http://127.0.0.1:{port}{path}"


def request_bridge(port: int, path: str, timeout: float, body: bytes | None = None) -> object:
    """Call the bridge with the localhost headers required by its origin checks."""
    url = bridge_url(port, path)
    headers = {"Host": "localhost", "Origin": "http://localhost"}
    if body is not None:
        headers["Content-Type"] = "application/json"
    # bridge_url restricts both the host/port range and endpoint path to fixed local values.
    request = Request(url, data=body, headers=headers, method="POST" if body is not None else "GET")  # NOSONAR
    try:
        # URL is loopback-only and its port was validated by bridge_url.
        with urlopen(request, timeout=timeout) as response:  # nosec B310
            return json.loads(response.read().decode("utf-8")) if body is not None else response.status
    except HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace").strip()
        raise AnalysisError(f"IDE bridge returned HTTP {error.code}{': ' + detail if detail else ''}") from error
    except URLError as error:
        raise AnalysisError(f"Cannot reach the SonarQube for IDE bridge: {error.reason}") from error
    except TimeoutError as error:
        raise AnalysisError("SonarQube for IDE bridge timed out") from error
    except json.JSONDecodeError as error:
        raise AnalysisError("IDE bridge returned invalid JSON") from error





def write_workspace(root: Path, cache: Path, database: Path, connection_id: str, project_key: str) -> Path:
    """Keep generated workspace state in the managed cache, never the source profile."""
    workspace = cache / "sonar.code-workspace"
    write_json(workspace, {"folders": [{"path": str(root)}], "settings": {
        "sonarlint.pathToCompileCommands": str(database), "sonarlint.connectedMode.project": {
            "connectionId": connection_id, "projectKey": project_key}}})
    return workspace





def normalized_findings(response: object) -> list[dict[str, object]]:
    """Validate the official bridge response shape while preserving Sonar fields."""
    if not isinstance(response, dict) or not isinstance(response.get("findings"), list):
        raise AnalysisError("IDE bridge response did not contain a findings array")
    findings = response["findings"]
    if not all(isinstance(finding, dict) for finding in findings):
        raise AnalysisError("IDE bridge response contained an invalid finding")
    return findings


def parse_changed_line_ranges(root: Path, diff: str) -> dict[Path, list[tuple[int, int]]]:
    """Parse inclusive added-line ranges from one zero-context Git diff."""
    ranges: dict[Path, list[tuple[int, int]]] = {}
    current: Path | None = None
    hunk = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")
    for line in diff.splitlines():
        if line.startswith("+++ "):
            name = line[4:]
            current = None if name == "/dev/null" else (root / name.removeprefix("b/")).resolve()
            continue
        match = hunk.match(line)
        if current is not None and match is not None:
            start = int(match.group(1))
            count = int(match.group(2) or "1")
            if count > 0:
                ranges.setdefault(current, []).append((start, start + count - 1))
    return ranges


def add_untracked_line_ranges(root: Path, files: Sequence[Path], ranges: dict[Path, list[tuple[int, int]]]) -> None:
    """Treat the complete contents of selected untracked files as changed."""
    tracked = set(paths_from_nul_output(run_git(root, "ls-files", "-z")))
    for path in files:
        if path.relative_to(root) in tracked:
            continue
        line_count = len(path.read_text(encoding="utf-8", errors="surrogateescape").splitlines())
        if line_count > 0:
            ranges[path] = [(1, line_count)]


def changed_line_ranges(root: Path, base: str, files: Sequence[Path]) -> dict[Path, list[tuple[int, int]]]:
    """Return inclusive added-line ranges, including the full contents of untracked files."""
    merge_base = run_git(root, "merge-base", "--", validated_git_ref(base), "HEAD").decode("utf-8").strip()
    relative = [str(path.relative_to(root)) for path in files]
    diff = run_git(root, "diff", "--unified=0", "--no-color", "--diff-filter=ACMR", merge_base, "--", *relative).decode(
        "utf-8", errors="surrogateescape"
    )
    ranges = parse_changed_line_ranges(root, diff)
    add_untracked_line_ranges(root, files, ranges)
    return ranges


def filter_findings_to_ranges(
    findings: Sequence[dict[str, object]], ranges: dict[Path, list[tuple[int, int]]]
) -> tuple[list[dict[str, object]], int]:
    """Legacy range helper; the managed workflow retains all file findings."""
    selected: list[dict[str, object]] = []
    for finding in findings:
        file_path = finding.get("filePath")
        text_range = finding.get("textRange")
        path = Path(file_path).resolve() if isinstance(file_path, str) else None
        if path not in ranges:
            continue
        if not isinstance(text_range, dict):
            selected.append(finding)
            continue
        start = text_range.get("startLine")
        end = text_range.get("endLine", start)
        if isinstance(start, int) and isinstance(end, int) and any(start <= last and end >= first for first, last in ranges[path]):
            selected.append(finding)
    return selected, len(findings) - len(selected)


def analyze_when_indexed(port: int, files: Sequence[Path], request_timeout: float, startup_timeout: float) -> list[dict[str, object]]:
    """Retry only the bridge's explicit not-yet-indexed startup response."""
    body = json.dumps({"fileAbsolutePaths": [str(path) for path in files]}).encode("utf-8")
    deadline = time.monotonic() + startup_timeout
    while True:
        try:
            response = request_bridge(port, BRIDGE_ANALYZE_PATH, request_timeout, body)
            return normalized_findings(response)
        except AnalysisError as error:
            if "No files were found to be indexed by SonarQube for IDE" not in str(error) or time.monotonic() >= deadline:
                raise
            time.sleep(0.5)























# Native

def ensure_supported() -> None:
    """Require the native ownership facilities; never fall back to PID killing."""
    if sys.platform == "win32":
        return
    if sys.platform.startswith("linux") and hasattr(os, "pidfd_open") and hasattr(signal, "pidfd_send_signal"):
        return
    raise AnalysisError("Managed quality processes require Linux with pidfd support or Windows")


def _windows_directory(variable: str) -> Path:
    value = os.environ.get(variable)
    if not value or not Path(value).is_absolute():
        raise AnalysisError(f"Windows quality tools require an absolute {variable} directory")
    return Path(value)


def default_code_user_data() -> Path:
    """Locate stable VS Code user data, not an editor or repository cache."""
    ensure_supported()
    if sys.platform == "win32":
        return _windows_directory("APPDATA") / "Code"
    return Path(os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config"))) / "Code"


def cache_home() -> Path:
    """Return the quality cache root (including the application namespace)."""
    ensure_supported()
    if sys.platform == "win32":
        return _windows_directory("LOCALAPPDATA") / "horo-quality"
    return Path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache"))) / "horo-quality"


def command_line_arguments(command: str) -> list[str]:
    """Parse native compiler commands without stripping Windows backslashes."""
    ensure_supported()
    if sys.platform != "win32":
        import shlex
        return shlex.split(command)
    if not command.strip():
        return []
    import ctypes
    from ctypes import wintypes
    shell = ctypes.WinDLL("shell32", use_last_error=True)
    parse = shell.CommandLineToArgvW
    parse.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_int)]
    parse.restype = ctypes.POINTER(wintypes.LPWSTR)
    free = ctypes.WinDLL("kernel32", use_last_error=True).LocalFree
    free.argtypes = [wintypes.HLOCAL]
    free.restype = wintypes.HLOCAL
    count = ctypes.c_int()
    arguments = parse(command, ctypes.byref(count))
    if not arguments:
        raise AnalysisError("Cannot parse Windows compiler command")
    try:
        return [arguments[index] for index in range(count.value)]
    finally:
        free(ctypes.cast(arguments, wintypes.HLOCAL))


def try_lock(stream) -> bool:
    """Acquire one stable byte; only native contention becomes False."""
    ensure_supported()
    if sys.platform == "win32":
        return _windows().try_lock(stream)
    else:
        import fcntl
        try:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return False
    return True


def unlock(stream) -> None:
    """Release precisely the range acquired by try_lock, irrespective of seek."""
    if sys.platform == "win32":
        _windows().unlock(stream)
    else:
        import fcntl
        fcntl.flock(stream, fcntl.LOCK_UN)


@functools.lru_cache(maxsize=1)
def _windows():
    """Bind explicitly sized Win32 handles; never truncate handles to int32."""
    import ctypes as c
    from ctypes import wintypes as w

    class BasicLimits(c.Structure):
        _fields_ = [("process_time", c.c_int64), ("job_time", c.c_int64),
                    ("flags", w.DWORD), ("min_ws", c.c_size_t), ("max_ws", c.c_size_t),
                    ("active", w.DWORD), ("affinity", c.c_size_t), ("priority", w.DWORD), ("scheduling", w.DWORD)]

    class IoCounters(c.Structure):
        _fields_ = [(name, c.c_uint64) for name in ("read_ops", "write_ops", "other_ops", "read_bytes", "write_bytes", "other_bytes")]

    class ExtendedLimits(c.Structure):
        _fields_ = [("basic", BasicLimits), ("io", IoCounters), ("process_memory", c.c_size_t),
                    ("job_memory", c.c_size_t), ("peak_process", c.c_size_t), ("peak_job", c.c_size_t)]

    class Overlapped(c.Structure):
        _fields_ = [("internal", c.c_size_t), ("internal_high", c.c_size_t),
                    ("offset", w.DWORD), ("offset_high", w.DWORD), ("event", w.HANDLE)]

    class ThreadEntry(c.Structure):
        _fields_ = [("size", w.DWORD), ("usage", w.DWORD), ("tid", w.DWORD), ("pid", w.DWORD),
                    ("base_priority", w.LONG), ("delta_priority", w.LONG), ("flags", w.DWORD)]

    kernel = c.WinDLL("kernel32", use_last_error=True)
    signatures = {
        "LockFileEx": ([w.HANDLE, w.DWORD, w.DWORD, w.DWORD, w.DWORD, c.POINTER(Overlapped)], w.BOOL),
        "UnlockFileEx": ([w.HANDLE, w.DWORD, w.DWORD, w.DWORD, c.POINTER(Overlapped)], w.BOOL),
        "CreateJobObjectW": ([c.c_void_p, w.LPCWSTR], w.HANDLE),
        "SetInformationJobObject": ([w.HANDLE, c.c_int, c.c_void_p, w.DWORD], w.BOOL),
        "AssignProcessToJobObject": ([w.HANDLE, w.HANDLE], w.BOOL),
        "TerminateJobObject": ([w.HANDLE, w.UINT], w.BOOL),
        "QueryInformationJobObject": ([w.HANDLE, c.c_int, c.c_void_p, w.DWORD, c.c_void_p], w.BOOL),
        "CloseHandle": ([w.HANDLE], w.BOOL),
        "OpenProcess": ([w.DWORD, w.BOOL, w.DWORD], w.HANDLE),
        "GetProcessTimes": ([w.HANDLE] + [c.POINTER(w.FILETIME)] * 4, w.BOOL),
        "TerminateProcess": ([w.HANDLE, w.UINT], w.BOOL),
        "WaitForSingleObject": ([w.HANDLE, w.DWORD], w.DWORD),
        "CreateToolhelp32Snapshot": ([w.DWORD, w.DWORD], w.HANDLE),
        "Thread32First": ([w.HANDLE, c.POINTER(ThreadEntry)], w.BOOL),
        "Thread32Next": ([w.HANDLE, c.POINTER(ThreadEntry)], w.BOOL),
        "OpenThread": ([w.DWORD, w.BOOL, w.DWORD], w.HANDLE),
        "ResumeThread": ([w.HANDLE], w.DWORD),
    }
    for name, (args, result) in signatures.items():
        function = getattr(kernel, name)
        function.argtypes, function.restype = args, result

    class Windows:
        def __init__(self):
            self.kernel = kernel

        def try_lock(self, stream):
            import msvcrt
            stream.flush()
            # OVERLAPPED.Offset is zero even if an append-mode stream is at EOF.
            overlapped = Overlapped()
            handle = msvcrt.get_osfhandle(stream.fileno())
            if not self.kernel.LockFileEx(handle, 3, 0, 1, 0, c.byref(overlapped)):
                code = c.get_last_error()
                if code == 33:  # ERROR_LOCK_VIOLATION, not ERROR_ACCESS_DENIED.
                    return False
                raise c.WinError(code)
            return True

        def unlock(self, stream):
            import msvcrt
            overlapped = Overlapped()
            handle = msvcrt.get_osfhandle(stream.fileno())
            self.check(self.kernel.UnlockFileEx(handle, 0, 1, 0, c.byref(overlapped)))

        def check(self, value):
            if not value:
                raise c.WinError(c.get_last_error())
            return value

        def close(self, handle):
            self.check(kernel.CloseHandle(handle))

        def open_process(self, pid, terminate=False):
            # QUERY_LIMITED_INFORMATION | SYNCHRONIZE, optionally TERMINATE.
            handle = kernel.OpenProcess(0x1000 | 0x100000 | int(terminate), False, pid)
            if not handle:
                code = c.get_last_error()
                if code == 87:
                    raise ProcessLookupError(pid)
                raise c.WinError(code)
            return handle

        def started(self, handle):
            times = [w.FILETIME() for _ in range(4)]
            self.check(kernel.GetProcessTimes(handle, *(c.byref(value) for value in times)))
            return str((times[0].dwHighDateTime << 32) | times[0].dwLowDateTime)

        def terminate(self, handle):
            if kernel.WaitForSingleObject(handle, 0) == 0:
                return
            self.check(kernel.TerminateProcess(handle, 1))
            if kernel.WaitForSingleObject(handle, 5000) != 0:
                raise OSError("Timed out waiting for pinned Windows process termination")

        def create_job(self):
            job = self.check(kernel.CreateJobObjectW(None, None))
            try:
                limits = ExtendedLimits()
                limits.basic.flags = 0x2000  # KILL_ON_JOB_CLOSE; no breakaway permission.
                self.check(kernel.SetInformationJobObject(job, 9, c.byref(limits), c.sizeof(limits)))
                return job
            except BaseException:
                self.close(job)
                raise

        def attach_and_resume(self, job, process):
            self.check(kernel.AssignProcessToJobObject(job, int(process._handle)))
            # CREATE_SUSPENDED prevents all user code (and child creation) until
            # this job owns the process. Popen closes its initial thread handle;
            # Toolhelp retrieves the sole initial thread while it is suspended.
            snapshot = kernel.CreateToolhelp32Snapshot(4, 0)  # SNAPTHREAD
            if snapshot == c.c_void_p(-1).value:
                raise c.WinError(c.get_last_error())
            threads = []
            try:
                entry = ThreadEntry()
                entry.size = c.sizeof(entry)
                available = kernel.Thread32First(snapshot, c.byref(entry))
                while available:
                    if entry.pid == process.pid:
                        threads.append(entry.tid)
                    available = kernel.Thread32Next(snapshot, c.byref(entry))
                if c.get_last_error() != 18:  # ERROR_NO_MORE_FILES
                    raise c.WinError(c.get_last_error())
                if len(threads) != 1:
                    raise OSError("Cannot identify sole suspended process thread")
                thread = self.check(kernel.OpenThread(2, False, threads[0]))
                try:
                    if kernel.ResumeThread(thread) != 1:
                        raise OSError("Unexpected suspended process thread state")
                finally:
                    self.close(thread)
            finally:
                self.close(snapshot)

        def release_job(self, job):
            # Successful CLI launchers intentionally leave a marked IDE alive.
            # Keep containment until successful communicate, then transfer its
            # lifetime to the session registry instead of killing it on close.
            limits = ExtendedLimits()
            self.check(kernel.SetInformationJobObject(job, 9, c.byref(limits), c.sizeof(limits)))

        def kill_job(self, job):
            self.check(kernel.TerminateJobObject(job, 1))
            class Accounting(c.Structure):
                _fields_ = [("user", c.c_int64), ("kernel", c.c_int64),
                            ("period_user", c.c_int64), ("period_kernel", c.c_int64),
                            ("faults", w.DWORD), ("total", w.DWORD), ("active", w.DWORD), ("terminated", w.DWORD)]
            deadline = time.monotonic() + 5
            while True:
                accounting = Accounting()
                self.check(kernel.QueryInformationJobObject(job, 1, c.byref(accounting), c.sizeof(accounting), None))
                if accounting.active == 0:
                    return
                if time.monotonic() >= deadline:
                    raise OSError("Timed out waiting for owned Windows job tree")
                time.sleep(0.01)

    return Windows()


def _command(argv: list[str], env: dict | None):
    """Resolve Windows executable shims explicitly; never interpolate a shell."""
    if not argv:
        raise AnalysisError("Quality command requires an executable")
    if sys.platform != "win32":
        return argv
    executable = shutil.which(argv[0], path=(env or os.environ).get("PATH"))
    if not executable:
        raise AnalysisError(f"Quality executable not found: {Path(argv[0]).name}")
    if Path(executable).suffix.lower() in {".cmd", ".bat"}:
        # cmd.exe expands even quoted percent/! variables and has no universal
        # argv escaping. Refuse such input instead of silently executing it.
        values = [executable, *argv[1:]]
        if any(any(char in value for char in '&|<>^%!"\r\n()') for value in values):
            raise AnalysisError("Unsafe Windows batch arguments; use the tool's native executable instead")
        interpreter = Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32/cmd.exe"
        return f'"{interpreter}" /d /v:off /s /c "' + " ".join(f'"{value}"' for value in values) + '"'
    return [executable, *argv[1:]]


def native_run(argv: list[str], root: Path, timeout: float, env: dict | None,
        cancelled: threading.Event, lifecycle: dict | None = None) -> subprocess.CompletedProcess[str]:
    """Bound an owned command; Windows trees cannot run before job assignment."""
    if lifecycle is not None:
        lifecycle["outcome"] = "no_escape"
    ensure_supported()
    if cancelled.is_set():
        raise AnalysisError("Quality command cancelled before launch")
    command = _command(argv, env)
    native = _windows() if sys.platform == "win32" else None
    job = None
    process = None
    completed = False
    try:
        options: dict = {"creationflags": 4} if native else {"start_new_session": True}
        job = native.create_job() if native else None
        if lifecycle is not None:
            lifecycle["outcome"] = "ambiguous"
        try:
            process = subprocess.Popen(command, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE,  # nosec B603
                                       text=True, encoding="utf-8" if native else None,
                                       errors="replace" if native else None, env=env, **options)
        except OSError:
            # Popen failed to create a child; unlike interrupted creation this
            # is positive no-launch evidence, not an empty discovery snapshot.
            if lifecycle is not None:
                lifecycle["outcome"] = "no_escape"
            raise
        if native:
            native.attach_and_resume(job, process)
        deadline = time.monotonic() + timeout
        while True:
            if cancelled.is_set():
                raise AnalysisError(f"Quality command cancelled: {Path(argv[0]).name}")
            if time.monotonic() >= deadline:
                raise AnalysisError(f"Quality command timed out: {Path(argv[0]).name}")
            try:
                stdout, stderr = process.communicate(timeout=max(0.01, min(0.1, deadline - time.monotonic())))
                completed = True
                return subprocess.CompletedProcess(argv, process.returncode, stdout, stderr)
            except subprocess.TimeoutExpired:
                pass
    except KeyboardInterrupt:
        raise AnalysisError(f"Quality command cancelled: {Path(argv[0]).name}") from None
    except OSError as error:
        # Do not print argv, environment, captured scanner text or credentials.
        raise AnalysisError(f"Cannot execute quality tool {Path(argv[0]).name}; check executable and native process permissions") from error
    finally:
        cleanup_error = None
        if job is not None:
            try:
                try:
                    if process is not None:
                        if completed:
                            native.release_job(job)
                        else:
                            native.kill_job(job)
                finally:
                    # Close before draining pipes: on any kill/release failure,
                    # KILL_ON_JOB_CLOSE remains the fail-closed backstop.
                    native.close(job)
            except OSError as error:
                cleanup_error = error
        if process is not None:
            try:
                try:
                    if native:
                        # Assignment failure leaves only the suspended leader;
                        # Popen's pinned process handle owns it, never a raw PID.
                        if process.poll() is None:
                            process.kill()
                    elif not completed:
                        with contextlib.suppress(ProcessLookupError):
                            os.killpg(process.pid, signal.SIGTERM)
                        try:
                            process.communicate(timeout=5)
                        except subprocess.TimeoutExpired:
                            pass
                        with contextlib.suppress(ProcessLookupError):
                            os.killpg(process.pid, signal.SIGKILL)
                finally:
                    process.communicate(timeout=5)
            except (OSError, subprocess.TimeoutExpired) as error:
                cleanup_error = error
            finally:
                for stream in (process.stdout, process.stderr):
                    if stream:
                        stream.close()
                if native:
                    process._handle.Close()
        if cleanup_error is not None:
            if lifecycle is not None:
                lifecycle["outcome"] = "ambiguous"
            raise AnalysisError("Owned quality process cleanup failed; check native process permissions") from cleanup_error
        if lifecycle is not None and native and process is not None and not completed:
            # Suspended creation + non-breakaway job + drained job and pinned
            # leader cleanup prove that this command left no surviving child.
            lifecycle["outcome"] = "no_escape"


def _psutil():
    try:
        import psutil
        return psutil
    except ImportError as error:
        raise AnalysisError("Windows quality ownership requires psutil; install scripts/requirements.txt with this Python interpreter") from error


def native_processes() -> dict[int, dict]:
    """Snapshot identities and only the two non-secret ownership markers.

    Inaccessible or exiting processes are omitted, never treated as owned. Every
    termination reopens and validates the exact kernel creation identity.
    """
    ensure_supported()
    markers = {"HORO_QUALITY_SESSION", "HORO_QUALITY_PROFILE"}
    result = {}
    if sys.platform == "win32":
        psutil = _psutil()
        native = _windows()
        try:
            candidates = psutil.process_iter()
            for process in candidates:
                handle = None
                try:
                    handle = native.open_process(process.pid)
                    started = native.started(handle)
                    with process.oneshot():
                        result[process.pid] = {"parent": process.ppid(), "started": started,
                                               "argv": process.cmdline(),
                                               "markers": {key: value for key, value in process.environ().items() if key in markers},
                                               "executable": process.exe()}
                except (psutil.NoSuchProcess, psutil.AccessDenied, ProcessLookupError, PermissionError):
                    continue
                except OSError as error:
                    if getattr(error, "winerror", None) in {5, 87, 299}:
                        continue
                    raise AnalysisError("Cannot inspect Windows process identity; check native process permissions") from error
                finally:
                    if handle is not None:
                        native.close(handle)
        except (psutil.Error, OSError) as error:
            raise AnalysisError("Cannot enumerate Windows processes; check native process permissions") from error
        return result
    for entry in Path("/proc").iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            fields = (entry / "stat").read_text().rsplit(") ", 1)[1].split()
            argv = (entry / "cmdline").read_bytes().split(b"\0")
            environment = {}
            for variable in (entry / "environ").read_bytes().split(b"\0"):
                key, separator, value = variable.partition(b"=")
                if separator and os.fsdecode(key) in markers:
                    environment[os.fsdecode(key)] = os.fsdecode(value)
            result[int(entry.name)] = {"parent": int(fields[1]), "started": fields[19],
                                       "argv": [os.fsdecode(arg) for arg in argv if arg],
                                       "markers": environment, "executable": os.readlink(entry / "exe")}
        except (OSError, ValueError, IndexError):
            continue
    return result


def native_listener_port(pids: set[int], ports: range) -> tuple[int, int] | None:
    """Match active TCP loopback LISTEN sockets to an owned process tree."""
    ensure_supported()
    if not pids:
        return None
    matches = set()
    if sys.platform == "win32":
        import ipaddress
        psutil = _psutil()
        try:
            for connection in psutil.net_connections(kind="tcp"):
                if connection.pid not in pids or connection.status != psutil.CONN_LISTEN or not connection.laddr:
                    continue
                address = ipaddress.ip_address(connection.laddr.ip)
                mapped = getattr(address, "ipv4_mapped", None)
                if connection.laddr.port in ports and (address.is_loopback or (mapped and mapped.is_loopback)):
                    matches.add((connection.pid, connection.laddr.port))
        except (psutil.Error, OSError, ValueError) as error:
            raise AnalysisError("Cannot verify Windows listening socket ownership; check native network permissions") from error
    else:
        import re
        sockets = {}
        try:
            for name in ("tcp", "tcp6"):
                for line in Path(f"/proc/net/{name}").read_text().splitlines()[1:]:
                    fields = line.split()
                    address, port_hex = fields[1].split(":")
                    port = int(port_hex, 16)
                    loopback = address in {"0100007F", "00000000000000000000000001000000",
                                           "0000000000000000FFFF00000100007F"}
                    if fields[3] == "0A" and port in ports and loopback:
                        sockets[fields[9]] = port
        except (OSError, ValueError, IndexError) as error:
            raise AnalysisError("Cannot verify Linux listening socket ownership through /proc/net") from error
        for pid in pids:
            try:
                for descriptor in Path(f"/proc/{pid}/fd").iterdir():
                    try:
                        target = os.readlink(descriptor)
                    except OSError:
                        continue
                    match = re.fullmatch(r"socket:\[(\d+)\]", target)
                    if match and match.group(1) in sockets:
                        matches.add((pid, sockets[match.group(1)]))
            except OSError:
                continue
    if len(matches) > 1:
        raise AnalysisError("Owned Code session has ambiguous Sonar listeners")
    return next(iter(matches), None)


def terminate_processes(pids: set[int], table: dict) -> bool:
    """Pin ALL supplied identities before signalling ANY of them.

    Caller must validate session ownership first. Missing/reused identities abort
    without signalling; vanished processes are harmless. Permissions fail closed.
    """
    ensure_supported()
    if any(not isinstance(pid, int) or pid <= 0 or not table.get(pid, {}).get("started") for pid in pids):
        return False
    native = _windows() if sys.platform == "win32" else None
    pinned = []
    try:
        for pid in sorted(pids):
            try:
                handle = native.open_process(pid, terminate=True) if native else os.pidfd_open(pid)
            except ProcessLookupError:
                continue
            pinned.append(handle)
            if native:
                current = native.started(handle)
            else:
                current = native_processes().get(pid, {}).get("started")
            if current != table[pid]["started"]:
                return False
        if native:
            for handle in pinned:
                native.terminate(handle)
        else:
            for descriptor in pinned:
                with contextlib.suppress(ProcessLookupError):
                    signal.pidfd_send_signal(descriptor, signal.SIGTERM)
            time.sleep(0.2)
            for descriptor in pinned:
                with contextlib.suppress(ProcessLookupError):
                    signal.pidfd_send_signal(descriptor, signal.SIGKILL)
        return True
    except OSError as error:
        raise AnalysisError("Cannot terminate verified quality processes; check native process permissions (no PID fallback is allowed)") from error
    finally:
        for handle in pinned:
            if native:
                native.close(handle)
            else:
                os.close(handle)


# Quality

CANCELLED = threading.Event()


def quality_run(argv: list[str], root: Path, timeout: float = 600, env: dict | None = None,
                lifecycle: dict | None = None) -> subprocess.CompletedProcess[str]:
    """Bound an owned native process tree, including cancellation cleanup."""
    if lifecycle is None:
        return native_run(argv, root, timeout, env, CANCELLED)
    return native_run(argv, root, timeout, env, CANCELLED, lifecycle)


def read_json(path: Path) -> dict:
    """Read an object or report an actionable input error."""
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise AnalysisError(f"Cannot read JSON: {path}") from error
    if not isinstance(value, dict):
        raise AnalysisError(f"Expected JSON object: {path}")
    return value


def write_json(path: Path, value: object) -> None:
    """Atomically publish private cache metadata."""
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    temporary = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    try:
        with temporary.open("w", encoding="utf-8") as stream:
            os.chmod(temporary, 0o600)
            json.dump(value, stream, ensure_ascii=True, indent=2)
            stream.write("\n")
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def digest(path: Path) -> str:
    """Hash content rather than relying on filesystem timestamps."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def identity(value: Path) -> str:
    """Give canonical paths a stable cache identity."""
    return hashlib.sha256(os.fsencode(value.resolve())).hexdigest()[:24]


def cache_directory(root: Path) -> Path:
    """Separate repositories by common Git directory, and each worktree by path."""
    common = Path(os.fsdecode(run_git(root, "rev-parse", "--git-common-dir")).strip())
    common = common if common.is_absolute() else root / common
    result = cache_home().resolve() / identity(common)
    if result.is_relative_to(root):
        raise AnalysisError("Quality cache must be outside the source tree")
    result.mkdir(parents=True, exist_ok=True, mode=0o700)
    return result


@contextlib.contextmanager
def lock(path: Path, timeout: float = 600):
    """Release native advisory locks on every exit, including process failure."""

    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    with path.open("a", encoding="utf-8") as stream:
        deadline = time.monotonic() + timeout
        while True:
            if CANCELLED.is_set():
                raise AnalysisError("Quality lease acquisition cancelled")
            if try_lock(stream):
                break
            if time.monotonic() >= deadline:
                raise AnalysisError(f"Timed out waiting for quality lease: {path.name}")
            time.sleep(0.1)
        try:
            yield
        finally:
            unlock(stream)


def repository_files(root: Path) -> list[Path]:
    """List first-party inputs with Git's NUL protocol; never follow outside symlinks."""
    paths = paths_from_nul_output(run_git(root, "ls-files", "-z", "--cached", "--others", "--exclude-standard"))
    excluded = {"deprecated", "vendor", "build", ".git", ".cmake", ".sonar", ".codacy/generated",
                "node_modules", "mock-studio", ".venv", "venv"}
    result = []
    for path in sorted(set(paths)):
        if any(part in excluded for part in path.parts) or str(path).startswith(".codacy/generated/"):
            continue
        absolute = root / path
        if absolute.is_file() and absolute.resolve().is_relative_to(root):
            result.append(path)
    return result


def fingerprint(root: Path) -> dict:
    """Cover headers/configuration and new inputs as well as selected source files."""
    return {"head": os.fsdecode(run_git(root, "rev-parse", "HEAD")).strip(),
            "status": hashlib.sha256(run_git(root, "status", "--porcelain=v1", "-z")).hexdigest(),
            "files": {str(path): digest(root / path) for path in repository_files(root)}}


def changed_ranges(root: Path, base_sha: str, paths: list[Path]) -> dict[str, list[tuple[int, int]]]:
    """Diff each NUL-selected path independently, avoiding quoted diff filenames."""
    import re

    tracked = set(paths_from_nul_output(run_git(root, "ls-files", "-z")))
    ranges = {}
    hunk = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")
    for path in paths:
        if path not in tracked:
            ranges[str(path)] = [(1, len((root / path).read_bytes().splitlines()))]
            continue
        diff = os.fsdecode(run_git(root, "diff", "--no-ext-diff", "--no-textconv", "--unified=0",
                                   "--no-color", base_sha, "--", str(path)))
        additions = []
        for line in diff.splitlines():
            match = hunk.match(line)
            if match and int(match.group(2) or 1):
                start = int(match.group(1))
                additions.append((start, start + int(match.group(2) or 1) - 1))
        ranges[str(path)] = additions
    return ranges


# Metrics

# nosec B405
# CPD XML declarations are rejected before parsing; no external entities are accepted.






def json_command(argv: list[str], root: Path, timeout: float = 600) -> tuple[dict, int]:
    """Require structured output even when a tool's exit code allows partial results."""
    result = quality_run(argv, root, timeout)
    try:
        payload = json.loads(result.stdout)
    except ValueError as error:
        raise AnalysisError(f"{Path(argv[0]).name} returned no usable JSON (exit {result.returncode})") from error
    if not isinstance(payload, dict):
        raise AnalysisError(f"{Path(argv[0]).name} returned a non-object report")
    return payload, result.returncode


def codacy_info(root: Path) -> dict:
    """Read descriptors from the installed CLI rather than hard-code tool routing."""
    payload, code = json_command(["codacy-analysis", "info", "--output-format", "json"], root, 90)
    if code != 0 or not isinstance(payload.get("tools"), list):
        raise AnalysisError("Cannot discover installed Codacy analyzers")
    return {tool["id"]: tool for tool in payload["tools"]}


def snapshot(root: Path, directory: Path) -> Path:
    """
    Copy inputs into a disposable mirror: CLI adapters write .codacy/generated.

    Never use hardlinks: a tool writing its inputs must not modify user files.
    Preserve relative local configuration paths in an independent Git index.
    """
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    if not (directory / ".git").is_dir():
        initialized = quality_run(["git", "init", "--quiet", str(directory)], directory)
        if initialized.returncode:
            raise AnalysisError("Cannot initialize the disposable Codacy input mirror")
    files = repository_files(root)
    manifest_path = directory.parent / "snapshot.json"
    previous = read_json(manifest_path) if manifest_path.exists() else {}
    current = {}
    for relative in files:
        source, destination = root / relative, directory / relative
        content_hash = digest(source)
        current[str(relative)] = content_hash
        if previous.get(str(relative)) != content_hash or not destination.is_file() or digest(destination) != content_hash:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
    for name in previous.keys() - current.keys():
        target = directory / name
        if target.resolve().is_relative_to(directory.resolve()):
            target.unlink(missing_ok=True)
    write_json(manifest_path, current)
    staged = quality_run(["git", "add", "--all", "--", "."], directory)
    if staged.returncode:
        raise AnalysisError("Cannot index the disposable Codacy input mirror")
    return directory


def codacy_check(root: Path, mirror: Path, selected: list[Path], config: dict, info: dict,
                 timeout: float) -> dict:
    """Run configured tools and account for every routed path and execution result."""
    extensions = {str(path): language(path) for path in selected}
    tools = []
    covered = set()
    unsupported = []
    for configured in config["tools"]:
        tool_id = configured["toolId"]
        descriptor = info.get(tool_id)
        if descriptor is None:
            unsupported.append({"toolId": tool_id, "reason": "missing_descriptor"})
            continue
        languages = {item.lower().replace("c++", "cpp").replace("c#", "csharp")
                     for item in descriptor.get("languages", [])}
        routed = {path for path, lang in extensions.items() if lang.lower() in languages}
        if tool_id == "Agentlinter":
            routed = {str(path) for path in selected if str(path).startswith((".codex/", ".github/instructions/"))
                      or path.name in {"AGENTS.md", "CLAUDE.md"}}
        if routed:
            tools.append(tool_id)
            covered.update(routed)
    skipped = [{"path": str(path), "reason": "no_configured_local_analyzer"}
               for path in selected if str(path) not in covered]
    if not selected:
        return {"status": "not_applicable", "issues": [], "skipped": [], "unsupported": []}
    if not tools:
        return {"status": "incomplete", "issues": [], "skipped": skipped, "unsupported": unsupported}
    argv = ["codacy-analysis", "analyze", "--output-format", "json", "--no-log", "--no-update-notifier",
            "--fail-if-missing", "--parallel-tools", "2", "--tool-timeout", str(int(timeout * 1000))]
    for tool in tools:
        argv.extend(["--tool", tool])
    # Literal paths containing glob characters must not be silently expanded by the CLI.
    if any(any(char in str(path) for char in "*?[") for path in selected):
        raise AnalysisError("Codacy --files cannot safely select paths containing glob metacharacters")
    argv.extend(["--files", *[str(path) for path in selected]])
    payload, code = json_command(argv, mirror, timeout)
    errors = execution_errors(payload, tools)
    capability = payload.get("capability", {})
    incomplete = code not in {0, 1} or bool(errors or skipped or unsupported or capability.get("unavailable"))
    payload.update(status="incomplete" if incomplete else ("issues_found" if payload.get("issues") else "clean"),
                   errors=errors, skipped=skipped, unsupported=unsupported, cliExitCode=code)
    return payload


def execution_errors(payload: dict, tools: list[str]) -> list:
    """Reject omitted tools, skipped inputs and partially routed executions."""
    executions = {item["toolId"]: item for item in payload.get("toolResults", [])}
    errors = list(payload.get("errors", []))
    reported_skips = payload.get("skippedFiles", [])
    if reported_skips:
        errors.append({"reason": "analyzer_skipped_files", "files": reported_skips})
    for tool in tools:
        result = executions.get(tool)
        if not result or result.get("status") != "success" or result.get("errorCount", 0) or not result.get("filesAnalyzed", 0):
            errors.append({"toolId": tool, "reason": "missing_or_incomplete_execution"})
    capability = payload.get("capability", {})
    for ready in capability.get("ready", []):
        execution = executions.get(ready["toolId"], {})
        if ready.get("filesRouted") != execution.get("filesAnalyzed"):
            errors.append({"toolId": ready["toolId"], "reason": "routed_file_count_mismatch",
                           "routed": ready.get("filesRouted"), "analyzed": execution.get("filesAnalyzed")})
    return errors


def language(path: Path) -> str:
    """Map repository file types to Codacy descriptor language identifiers."""
    if path.suffix.lower() in SUPPORTED_SUFFIXES:
        return "C" if path.suffix.lower() == ".c" else "CPP"
    return {".py": "Python", ".md": "Markdown", ".json": "JSON", ".yml": "YAML", ".yaml": "YAML",
            ".sh": "Shell", ".css": "CSS", ".html": "HTML", ".js": "Javascript",
            ".ts": "Typescript", ".jsx": "Javascript", ".tsx": "Typescript"}.get(path.suffix.lower(), "unknown")


def excluded(path: Path, config: dict) -> bool:
    """Honor the checked-in Codacy snapshot's excludes."""
    return any(fnmatch.fnmatchcase(path.as_posix(), pattern) for pattern in config.get("exclude", []))


def lizard_executable(installation: str | None = None) -> Path:
    """Match the current Codacy adapter's managed-venv-first resolution."""
    runtime = Path.home() / ".codacy/runtimes/lizard-1/venv"
    managed = runtime / ("Scripts/lizard.exe" if os.name == "nt" else "bin/lizard")
    if installation != "global" and managed.is_file():
        return managed
    executable = shutil.which("lizard")
    if executable is None:
        raise AnalysisError("Lizard is unavailable; prepare Codacy analyzers first")
    return Path(executable)


def complexity(root: Path, files: list[Path], capability: dict, timeout: float) -> dict:
    """Measure raw CCN totals with the same executable/version as the issue pass."""
    if not files:
        return {"status": "not_applicable", "files": []}
    ready = next((item for item in capability.get("ready", []) if item.get("toolId") == "Lizard"), None)
    if ready is None:
        raise AnalysisError("No verified Codacy Lizard execution is available for complexity")
    executable = lizard_executable(ready.get("installation"))
    version = quality_run([str(executable), "--version"], root, timeout)
    if version.returncode or version.stdout.strip() != ready.get("version"):
        raise AnalysisError("Complexity Lizard version differs from the Codacy issue analyzer")
    result = quality_run([str(executable), "--csv", *[str(root / path) for path in files]], root, timeout)
    if result.returncode != 0 or result.stderr.strip():
        raise AnalysisError("Lizard could not complete raw complexity analysis")
    totals = {str(path): {"path": str(path), "cyclomaticComplexity": 0, "functions": []} for path in files}
    for row in csv.reader(io.StringIO(result.stdout)):
        if len(row) != 11:
            raise AnalysisError("Unsupported Lizard CSV format")
        path = str(Path(row[6]).resolve().relative_to(root))
        if path not in totals:
            raise AnalysisError("Lizard returned a file outside the selected set")
        function = {"name": row[7], "cyclomaticComplexity": int(row[1]), "nloc": int(row[0]),
                    "startLine": int(row[9]), "endLine": int(row[10])}
        totals[path]["functions"].append(function)
        totals[path]["cyclomaticComplexity"] += function["cyclomaticComplexity"]
    return {"status": "complete", "advisory": True, "version": ready["version"], "files": list(totals.values())}


def parse_clones(xml: str, mirror: Path, selected: set[str]) -> list[dict]:
    """Retain all occurrences of any clone touching a selected file."""
    if "<!DOCTYPE" in xml.upper() or "<!ENTITY" in xml.upper():
        raise AnalysisError("CPD returned unexpected XML declarations")
    try:
        document = element_tree.fromstring(xml)  # nosec B314
    except element_tree.ParseError as error:
        raise AnalysisError("CPD returned invalid XML") from error
    if document.tag != "pmd-cpd":
        raise AnalysisError("CPD returned an unexpected report root")
    if document.findall("error") or document.findall("processing-error"):
        raise AnalysisError("CPD reported skipped files or tokenization errors")
    clones = []
    for clone in document.findall("duplication"):
        occurrences = []
        for source in clone.findall("file"):
            path = str(Path(source.attrib["path"]).resolve().relative_to(mirror))
            start = int(source.attrib["line"])
            occurrences.append({"path": path, "startLine": start,
                                "endLine": start + int(clone.attrib["lines"]) - 1})
        if any(item["path"] in selected for item in occurrences):
            clones.append({"tokens": int(clone.attrib["tokens"]), "lines": int(clone.attrib["lines"]),
                           "occurrences": occurrences})
    return clones


def duplication(root: Path, mirror: Path, selected: list[Path], config: dict, info: dict,
                timeout: float) -> dict:
    """Run CPD over the full active C/C++ corpus and report selected-file matches."""
    if not selected:
        return {"status": "not_applicable", "clones": []}
    descriptor = info.get("PMD", {})
    version = descriptor.get("installedVersion")
    binary = "cpd.bat" if os.name == "nt" else "run.sh"
    executable = Path.home() / f".codacy/tools/PMD/pmd-bin-{version}/bin" / binary
    java = Path.home() / ".codacy/runtimes/java-21/bin" / ("java.exe" if os.name == "nt" else "java")
    if not executable.is_file():
        raise AnalysisError("Codacy-managed PMD CPD is missing; prepare the PMD analyzer first")
    env = dict(os.environ)
    if java.is_file():
        env["JAVA_HOME"] = str(java.parent.parent)
        env["PATH"] = f"{java.parent}{os.pathsep}{env.get('PATH', '')}"
    # Every path is explicit so generated caches, third-party sources and deleted files cannot enter CPD.
    corpus = [path for path in repository_files(root) if path.suffix.lower() in SUPPORTED_SUFFIXES and not excluded(path, config)]
    if any("\n" in str(path) or "\r" in str(path) for path in corpus):
        raise AnalysisError("CPD file lists cannot represent filenames containing newlines")
    filelist = mirror.parent / "cpd-files.txt"
    filelist.write_text("\n".join(str(mirror / path) for path in corpus) + "\n", encoding="utf-8")
    prefix = [str(executable)] if os.name == "nt" else [str(executable), "cpd"]
    result = quality_run([*prefix, "--minimum-tokens", "50", "--language", "cpp",
                  "--ignore-literals", "--ignore-annotations", "--skip-lexical-errors",
                  "--file-list", str(filelist), "--format", "xml"], mirror, timeout, env)
    if result.returncode not in {0, 4} or result.stderr.strip():
        raise AnalysisError("CPD failed or reported skipped inputs; inspect analyzer setup")
    clones = parse_clones(result.stdout, mirror, {str(path) for path in selected})
    return {"status": "complete", "advisory": True, "estimate": True, "version": version,
            "minimumTokens": 50, "corpusFiles": len(corpus), "clones": clones}


def secrets(root: Path, files: list[Path], timeout: float) -> dict:
    """
    Run only the CLI's local secrets scanner.

    Do not retain scanner text: it can contain secret values. Exit status and a
    local inspection command are retained instead of leaking them into reports.
    """
    if not files:
        return {"status": "not_applicable", "issues": []}
    result = quality_run(["sonar", "analyze", "secrets", *[str(root / path) for path in files]], root, timeout)
    if result.returncode not in {0, 51}:
        raise AnalysisError(f"Sonar secrets scan failed (exit {result.returncode})")
    if result.returncode == 0 and "No secrets found" not in result.stdout:
        raise AnalysisError("Secrets CLI did not confirm completion")
    return {"status": "issues_found" if result.returncode else "clean", "submitted": [str(path) for path in files],
            "issues": [{"message": "Secrets detected; inspect with sonar analyze secrets <selected paths> locally"}]
            if result.returncode else [], "cliExitCode": result.returncode}


# Session

def session_processes() -> dict[int, dict]:
    """Read process identity without exposing arguments to reports."""
    return native_processes()


def has_profile(process: dict, profile: Path) -> bool:
    """Match a complete argument, never a substring of an unrelated command."""
    argv = process["argv"]
    matched = False
    for index, arg in enumerate(argv):
        if arg.startswith("--user-data-dir="):
            value = arg.removeprefix("--user-data-dir=")
        elif arg == "--user-data-dir":
            if index + 1 >= len(argv):
                return False
            value = argv[index + 1]
        else:
            continue
        if not same_path(value, str(profile)):
            return False
        matched = True
    return matched


def same_path(first: str, second: str) -> bool:
    """Normalize Windows separators/case, never compare path substrings."""
    if sys.platform == "win32":
        return first.replace("\\", "/").casefold() == second.replace("\\", "/").casefold()
    return first == second


def owned(record: dict, table: dict[int, dict]) -> bool:
    """Reject exited/reused PIDs and records no longer naming our profile."""
    process = table.get(record.get("pid"))
    if not process or process["started"] != record.get("started") or not record.get("nonce"):
        return False
    if not same_path(process.get("executable", ""), record.get("executable", "")):
        return False
    if record.get("title"):
        if sys.platform == "win32" or process["argv"] != [record["title"]]:
            return False
    elif not has_profile(process, Path(record.get("profile", ""))):
        return False
    return any(table[pid].get("markers", {}).get("HORO_QUALITY_SESSION") == record["nonce"]
               and same_path(table[pid].get("markers", {}).get("HORO_QUALITY_PROFILE", ""), record["profile"])
               for pid in descendants(record["pid"], table))


def ancestry_edge(parent: dict, child: dict) -> bool:
    """A reused Windows parent PID cannot adopt a pre-existing child."""
    if sys.platform != "win32":
        return True
    try:
        return int(parent["started"]) <= int(child["started"])
    except (KeyError, ValueError, TypeError):
        return False


def descendants(pid: int, table: dict[int, dict]) -> set[int]:
    """Collect only children of the verified Code main process."""
    result = {pid}
    while True:
        added = {child for child, data in table.items() if data["parent"] in result
                 and ancestry_edge(table[data["parent"]], data)} - result
        if not added:
            return result
        result.update(added)


def session_listener_port(pids: set[int]) -> tuple[int, int] | None:
    """Match loopback listening sockets to file descriptors in the owned tree."""
    return native_listener_port(pids, BRIDGE_PORTS)


def terminate(record: dict) -> bool:
    """Verify session ownership before pinning native process identities."""
    if not record:
        return False
    table = session_processes()
    if not owned(record, table):
        return False
    pids = descendants(record["pid"], table)
    if record.get("listenerPid") is not None:
        listener = table.get(record["listenerPid"])
        if (not listener or listener["started"] != record.get("listenerStarted")
                or session_listener_port(pids) != (record["listenerPid"], record.get("port"))):
            return False
    return terminate_processes(pids, table)


def resolve_pending_launch(record: dict) -> dict:
    """Recover exact identities after either launcher step failed or was cancelled."""
    if not record.get("pending"):
        return record
    main = discover_main(session_processes(), Path(record["profile"]), Path(record["extension"]),
                         Path(record["workspace"]), record["nonce"])
    if len(main) != 1:
        return record
    pid, process, _ = main[0]
    result = dict(record, pid=pid, started=process["started"], executable=process["executable"],
                  isolationVersion=4, pending=False)
    if len(process["argv"]) == 1:
        result["title"] = process["argv"][0]
    return result


def retire(record: dict) -> bool:
    """Keep a live but unverifiable slot reserved instead of orphaning it."""
    if not record:
        return False
    if record.get("pending") and record.get("launchOutcome") == "no_escape":
        return False
    record = resolve_pending_launch(record)
    if record.get("pending"):
        raise AnalysisError("Managed launch ownership is not yet verifiable; slot remains reserved")
    stopped = terminate(record)
    process = session_processes().get(record.get("pid"))
    if not stopped and process and process["started"] == record.get("started"):
        raise AnalysisError("Managed session is still alive but ownership cannot be verified; slot remains reserved")
    return stopped


def copy_encrypted_credentials(profile: Path, source: Path) -> None:
    """Copy only Sonar encrypted keychain references from a read-only database."""
    storage = profile / "User/globalStorage"
    source_db = source / "User/globalStorage/state.vscdb"
    if source_db.is_file() and not (storage / "state.vscdb").exists():
        with sqlite3.connect(f"{source_db.as_uri()}?mode=ro", uri=True) as original:
            encrypted = []
            for key, value in original.execute("SELECT key, value FROM ItemTable WHERE key LIKE 'secret://%'"):
                try:
                    metadata = json.loads(key.removeprefix("secret://"))
                except ValueError:
                    continue
                if (isinstance(metadata, dict) and isinstance(metadata.get("extensionId"), str)
                        and metadata["extensionId"].lower() == "sonarsource.sonarlint-vscode"):
                    encrypted.append((key, value))
        if not encrypted:
            return
        if sys.platform == "win32":
            prepare_windows_encryption(profile, source, encrypted)
        else:
            validate_secretstorage_records(profile, encrypted)
        with sqlite3.connect(storage / "state.vscdb") as destination:
            destination.execute("CREATE TABLE ItemTable (key TEXT UNIQUE ON CONFLICT REPLACE, value BLOB)")
            destination.executemany("INSERT INTO ItemTable VALUES (?, ?)", encrypted)
        os.chmod(storage / "state.vscdb", 0o600)


def validate_secretstorage_records(profile: Path, encrypted: list[tuple]) -> None:
    """Accept only VS Code encrypted Buffer records, not plaintext/weak storage."""
    try:
        for _, value in encrypted:
            envelope = json.loads(value)
            if not isinstance(envelope, dict) or set(envelope) != {"type", "data"}:
                raise ValueError("Unsupported SecretStorage envelope")
            data = envelope["data"]
            if (envelope["type"] != "Buffer" or not isinstance(data, list)
                    or not all(type(byte) is int and 0 <= byte <= 255 for byte in data)):
                raise ValueError("Unsupported SecretStorage envelope")
            ciphertext = bytes(data)
            if sys.platform == "win32":
                valid = ciphertext.startswith(b"v10") and len(ciphertext) >= 31
            else:
                # Linux v10 uses Chromium's hardcoded obfuscation key. Require
                # the v11 OS-keychain format; never import basic-text storage.
                valid = (ciphertext.startswith(b"v11") and len(ciphertext) >= 19
                         and (len(ciphertext) - 3) % 16 == 0)
            if not valid:
                raise ValueError("Unsupported SecretStorage encryption")
    except (ValueError, TypeError, KeyError):
        raise AnalysisError(
            f"Cannot safely import Sonar SecretStorage; authenticate in the managed profile at {profile} "
            "using the OS keychain (never put tokens in settings.json)") from None


def prepare_windows_encryption(profile: Path, source: Path, encrypted: list[tuple]) -> None:
    """Carry only the DPAPI-wrapped profile key needed by Electron v10 records.

    VS Code encrypts SecretStorage with Electron safeStorage. On Windows its v10
    ciphertext depends on os_crypt.encrypted_key in Local State, not just the OS
    account. Never copy the rest of Local State or replace a managed profile key.
    Unknown formats need authentication in the managed profile, not plaintext.
    """
    message = (f"Cannot safely import Windows Sonar SecretStorage; authenticate in the managed profile "
               f"at {profile} and retry (never put tokens in settings.json)")
    try:
        validate_secretstorage_records(profile, encrypted)
        state = read_json(source / "Local State")
        key = state["os_crypt"]["encrypted_key"]
        wrapped = base64.b64decode(key, validate=True)
        if not wrapped.startswith(b"DPAPI") or len(wrapped) <= 5:
            raise ValueError("Unsupported encryption key")
        target = profile / "Local State"
        if target.exists():
            managed_encryption = read_json(target).get("os_crypt")
            if not isinstance(managed_encryption, dict) or managed_encryption.get("encrypted_key") != key:
                raise ValueError("Managed profile already has its own key")
        else:
            write_json(target, {"os_crypt": {"encrypted_key": key}})
    except (AnalysisError, ValueError, TypeError, KeyError, binascii.Error):
        raise AnalysisError(message) from None


def selected_extension(extensions: Path) -> Path:
    """Use the same installed extension selection for seeding and reuse."""
    installed = sorted(extensions.glob("sonarsource.sonarlint-vscode-*/package.json"),
                       key=lambda path: (path.stat().st_mtime_ns, path.parent.name), reverse=True)
    if not installed:
        raise AnalysisError("SonarQube for IDE is not installed in the supplied extensions directory")
    return installed[0].parent


def seed_profile(profile: Path, source: Path, extensions: Path) -> Path:
    """
    Copy connection metadata and only Sonar's encrypted SecretStorage records.

    The source settings/database are read-only. Plaintext tokens are never copied;
    keychain decryption remains VS Code's responsibility. Windows imports only
    the DPAPI-wrapped profile key; no mutable workspace/trust/analyzer state is
    copied from the source. A managed-window login remains private and reusable.
    """
    user = profile / "User"
    user.mkdir(parents=True, exist_ok=True, mode=0o700)
    settings_path = source / "User/settings.json"
    try:
        settings = read_json(settings_path) if settings_path.exists() else {}
    except AnalysisError as error:
        raise AnalysisError(f"A JSON VS Code settings file is required at {settings_path}") from error
    # Trust stays enabled; only the user may approve the managed workspace.
    copied = {"security.workspace.trust.enabled": True, "extensions.autoUpdate": False,
              "extensions.autoCheckUpdates": False, "sonarlint.output.showVerboseLogs": True,
              "sonarlint.automaticAnalysis": False, "window.restoreWindows": "none",
              "telemetry.telemetryLevel": "off", "workbench.startupEditor": "none"}
    # The language server otherwise shares ~/.sonarlint even across Code profiles.
    sonar_home = profile.parent / "sonar-home"
    sonar_home.mkdir(exist_ok=True, mode=0o700)
    copied["sonarlint.ls.vmargs"] = f'"-Duser.home={sonar_home}"'
    exclusions = {f"**/{name}/**": True for name in
                  ("build", "cmake-build-*", "vendor", "deprecated", ".git", "node_modules", ".venv")}
    copied["files.exclude"] = exclusions
    copied["files.watcherExclude"] = exclusions
    copied["search.exclude"] = exclusions
    managed_path = user / "settings.json"
    managed_settings = read_json(managed_path) if managed_path.exists() else {}
    for kind in ("sonarcloud", "sonarqube"):
        key = f"sonarlint.connectedMode.connections.{kind}"
        # A login performed in the managed window must survive the next launch.
        connections = managed_settings.get(key) or settings.get(key, [])
        copied[key] = [{k: v for k, v in connection.items() if k in
                        {"connectionId", "organizationKey", "serverUrl", "region", "disableNotifications"}}
                       for connection in connections]
    write_json(user / "settings.json", copied)
    storage = user / "globalStorage"
    storage.mkdir(exist_ok=True, mode=0o700)
    installed = selected_extension(extensions)
    extension_dir = profile.parent / "extensions"
    extension_dir.mkdir(exist_ok=True, mode=0o700)
    link = extension_dir / installed.name
    copy_extension(installed, link)
    copy_encrypted_credentials(profile, source)
    return link


def native_path(path: Path) -> Path:
    """Use extended-length Win32 paths without changing global OS policies."""
    if os.name != "nt":
        return path
    value = str(path.resolve())
    if value.startswith("\\\\?\\"):
        return Path(value)
    return Path("\\\\?\\UNC\\" + value[2:] if value.startswith("\\\\") else "\\\\?\\" + value)


def remove_extension_tree(path: Path) -> None:
    """Remove only an owned cache tree, including Windows read-only JRE files."""
    def retry_readonly(operation, filename, error):
        if sys.platform != "win32" or not isinstance(error[1], PermissionError):
            raise error[1]
        candidate = Path(filename)
        if candidate.is_symlink():
            raise error[1]
        candidate.chmod(candidate.stat().st_mode | stat.S_IWRITE)
        operation(filename)
    shutil.rmtree(path, onerror=retry_readonly)


def copy_extension(source: Path, target: Path) -> None:
    """Publish a complete immutable extension tree; never reuse an unknown partial copy.

    Stage beside the target, so rename is same-volume. Only a completed copy gets
    a marker; all files (including deeply nested licenses) are retained. Legacy
    unmarked targets are replaced. A failed staging operation leaves no target.
    """
    destination = native_path(target)
    marker = destination / ".horo-copy-complete"
    if marker.is_file():
        return
    staging = native_path(target.with_name(".copy-" + uuid.uuid4().hex))
    try:
        shutil.copytree(native_path(source), staging)
        (staging / ".horo-copy-complete").write_text("1\n", encoding="ascii")
        if destination.is_symlink():
            destination.unlink()
        elif destination.exists():
            remove_extension_tree(destination)
        staging.rename(destination)
    finally:
        if staging.exists():
            remove_extension_tree(staging)


def logs(profile: Path) -> list[Path]:
    """Return only Sonar logs beneath the private profile."""
    return sorted(profile.glob("logs/**/SonarQube for IDE*.log"), key=lambda path: path.stat().st_mtime_ns)


def log_offsets(profile: Path) -> dict[int, int]:
    """Record byte offsets before submitting a request."""
    return {path.stat().st_ino: path.stat().st_size for path in logs(profile)}


def log_delta(profile: Path, offsets: dict[int, int]) -> str:
    """Read only diagnostics emitted since the current request began."""
    parts = []
    for path in logs(profile):
        with path.open("rb") as stream:
            stream.seek(offsets.get(os.fstat(stream.fileno()).st_ino, 0))
            parts.append(stream.read().decode("utf-8", errors="replace"))
    return "\n".join(parts)


def discover_main(table: dict, profile: Path, extension: Path, workspace: Path, nonce: str) -> list:
    """Identify the launched Code ancestor through its marked extension host."""
    main = []
    for witness_pid, witness in table.items():
        if (not same_path(witness.get("markers", {}).get("HORO_QUALITY_PROFILE", ""), str(profile))
                or witness.get("markers", {}).get("HORO_QUALITY_SESSION") != nonce):
            continue
        parent = witness_pid
        seen = set()
        while parent in table and parent not in seen:
            seen.add(parent)
            process = table[parent]
            expected_title = (f"{process['executable']} --user-data-dir --extensions-dir "
                              f"--extensionDevelopmentPath --new-window {profile} "
                              f"{profile.parent / 'extensions'} {extension} {workspace}")
            executable_name = process["executable"].replace("\\", "/").rsplit("/", 1)[-1]
            code_main = (executable_name.casefold() == "code.exe" if sys.platform == "win32"
                         else executable_name == "code")
            title_match = sys.platform.startswith("linux") and process["argv"] == [expected_title]
            # Electron's CLI uses the same executable/profile/markers as its
            # GUI child, but its positive script role is never a Code main.
            cli_script = process["executable"].replace("\\", "/").rsplit("/", 1)[0] + "/resources/app/out/cli.js"
            cli_launcher = len(process["argv"]) > 1 and same_path(process["argv"][1], cli_script)
            if code_main and not cli_launcher and (has_profile(process, profile) or title_match):
                if not any(arg == "--type" or arg.startswith("--type=") for arg in process["argv"]):
                    if not any(candidate[0] == parent for candidate in main):
                        main.append((parent, process, nonce))
            ancestor = table.get(process["parent"])
            if ancestor is None or not ancestry_edge(ancestor, process):
                break
            parent = process["parent"]
    return main


def launch_profile(root: Path, profile: Path, workspace: Path, source: Path, extensions: Path,
                   timeout: float, activation_file: Path, register=None,
                   lifecycle: dict | None = None) -> tuple[str, Path]:
    """Launch the released Sonar extension in a private Code development host."""
    extension = seed_profile(profile, source, extensions)
    nonce = uuid.uuid4().hex
    env = dict(os.environ)
    env.pop("VSCODE_IPC_HOOK_CLI", None)
    env["HORO_QUALITY_SESSION"] = nonce
    env["HORO_QUALITY_PROFILE"] = str(profile)
    if register is not None:
        register(nonce, extension)
    # Force activation of the existing released extension in this isolated host;
    # newer Code builds can otherwise inherit a shared disabled-extension list.
    attempts = []
    try:
        for arguments in (["--extensionDevelopmentPath", str(extension), "--new-window", str(workspace)],
                          ["--reuse-window", "--goto", str(activation_file)]):
            attempt = {"outcome": "ambiguous"}
            attempts.append(attempt)
            quality_run(["code", "--user-data-dir", str(profile), "--extensions-dir",
                         str(profile.parent / "extensions"), *arguments], root, timeout, env, attempt)
    finally:
        if lifecycle is not None:
            lifecycle["outcome"] = ("no_escape" if attempts and all(
                item["outcome"] == "no_escape" for item in attempts) else "ambiguous")
    return nonce, extension


class SessionPool:

    """Lease up to two isolated sessions per common Git repository."""

    def __init__(self, cache: Path, root: Path, timeout: float = 600):
        """Scope capacity to the repository and sessions to individual worktrees."""
        self.directory = cache / "sessions"
        self.directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.root = root
        self.timeout = timeout

    def acquire(self):
        """Claim an idle slot or wait while both sessions are leased."""
        deadline = time.monotonic() + self.timeout
        stream = None
        while stream is None:
            if CANCELLED.is_set():
                raise AnalysisError("Sonar session acquisition cancelled")
            with lock(self.directory / "registry.lock", self.timeout):
                records = []
                for slot in range(2):
                    path = self.directory / f"{slot}.json"
                    record = read_json(path) if path.exists() else {}
                    records.append((slot, record))
                records.sort(key=lambda item: (item[1].get("worktree") != str(self.root),
                                               item[1].get("usedAt", 0)))
                for slot, record in records:
                    candidate = (self.directory / f"{slot}.lock").open("a", encoding="utf-8")
                    try:
                        acquired = try_lock(candidate)
                    except BaseException:
                        candidate.close()
                        raise
                    if not acquired:
                        candidate.close()
                        continue
                    if record.get("worktree") != str(self.root):
                        try:
                            retire(record)
                        except BaseException:
                            unlock(candidate)
                            candidate.close()
                            raise
                        record = {}
                    stream = candidate
                    break
            if stream is None:
                if time.monotonic() >= deadline:
                    raise AnalysisError("Timed out waiting for one of two Sonar sessions")
                time.sleep(0.1)
        return slot, record, stream

    @contextlib.contextmanager
    def lease(self):
        """Release the lease and retire interrupted analyses on every exit."""
        slot, record, stream = self.acquire()
        try:
            yield slot, record
        except BaseException:
            # Abandon the server too: cancelling an HTTP client does not cancel
            # the IDE's queued analysis, which must not leak into the next lease.
            path = self.directory / f"{slot}.json"
            if path.exists():
                retire(read_json(path))
                path.unlink()
            raise
        finally:
            try:
                path = self.directory / f"{slot}.json"
                if path.exists():
                    record = read_json(path)
                    record["usedAt"] = time.time()
                    write_json(path, record)
            finally:
                try:
                    unlock(stream)
                finally:
                    stream.close()

    def open(self, slot: int, record: dict, workspace: Path, source: Path, extensions: Path,
             startup_timeout: float, activation_file: Path) -> dict:
        """Validate an existing session or launch one with owned process discovery."""
        profile = self.directory / identity(self.root) / "profile"
        table = session_processes() if record else {}
        if record and record.get("isolationVersion") == 4 and owned(record, table):
            installed = selected_extension(extensions)
            expected_extension = profile.parent / "extensions" / installed.name
            extension_matches = (same_path(record.get("extension", ""), str(expected_extension))
                                 and record.get("extensionVersion") == read_json(installed / "package.json").get("version"))
            listener = session_listener_port(descendants(record["pid"], table))
            if (extension_matches and listener and listener == (record.get("listenerPid"), record.get("port"))
                    and table[listener[0]]["started"] == record.get("listenerStarted")):
                request_bridge(listener[1], BRIDGE_STATUS_PATH, 2)
                return record
            retire(record)
        elif record:
            retire(record)
        record = {}
        path = self.directory / f"{slot}.json"
        pending = {}
        lifecycle = {}

        def register(nonce, extension):
            pending.update(nonce=nonce, profile=str(profile), extension=str(extension),
                           extensionVersion=read_json(extension / "package.json").get("version"),
                           workspace=str(workspace), worktree=str(self.root), pending=True,
                           launchOutcome="ambiguous")
            write_json(path, pending)

        deadline = time.monotonic() + startup_timeout
        try:
            nonce, extension = launch_profile(self.root, profile, workspace, source, extensions,
                                              startup_timeout, activation_file, register, lifecycle)
            main = []
            while time.monotonic() < deadline:
                if CANCELLED.is_set():
                    raise AnalysisError("Sonar startup cancelled")
                table = session_processes()
                main = discover_main(table, profile, extension, workspace, nonce)
                if len(main) > 1:
                    # Code's activation invocation can briefly run a second GUI
                    # before forwarding to the first. Accept nothing until the
                    # topology is unique; persistent independent mains fail closed.
                    time.sleep(0.2)
                    continue
                if main:
                    pid, process, launch_nonce = main[0]
                    record = {"pid": pid, "started": process["started"], "profile": str(profile), "nonce": launch_nonce,
                              "isolationVersion": 4,
                              "extension": str(extension), "extensionVersion": pending.get("extensionVersion"),
                              "executable": process["executable"],
                              "worktree": str(self.root), "workspace": str(workspace), "usedAt": time.time()}
                    if len(process["argv"]) == 1:
                        record["title"] = process["argv"][0]
                    # A provisional singleton is not the complete launch outcome.
                    # Keep the durable pending reservation until its bridge is verified.
                    listener = session_listener_port(descendants(pid, table))
                    if listener:
                        record.update(listenerPid=listener[0], port=listener[1],
                                      listenerStarted=table[listener[0]]["started"])
                        request_bridge(listener[1], BRIDGE_STATUS_PATH, 2)
                        write_json(self.directory / f"{slot}.json", record)
                        return record
                time.sleep(0.2)
            if len(main) > 1:
                raise AnalysisError("Managed session has ambiguous owned Code main processes; refusing to guess")
            raise AnalysisError(
                f"Managed VS Code has no verified Sonar bridge. Prerequisites: review workspace trust "
                f"and authenticate Sonar connected mode in the managed profile at {profile}; "
                "close the managed window before retrying. Trust remains enabled; inspect its "
                "extension logs if these prerequisites are already satisfied.")
        except BaseException:
            if pending:
                # Reconcile the whole launch, not an earlier singleton snapshot:
                # additional independent mains may have appeared before failure.
                if lifecycle.get("outcome") == "no_escape":
                    pending["launchOutcome"] = "no_escape"
                record = pending if pending.get("launchOutcome") == "no_escape" else resolve_pending_launch(pending)
                write_json(path, record)
                if record.get("launchOutcome") == "no_escape":
                    path.unlink(missing_ok=True)
                    raise
            if terminate(record):
                path.unlink(missing_ok=True)
            raise

    def stop(self) -> list[dict]:
        """Leave busy sessions and all unrelated user processes untouched."""
        result = []
        with lock(self.directory / "registry.lock", self.timeout):
            for slot in range(2):
                with (self.directory / f"{slot}.lock").open("a", encoding="utf-8") as stream:
                    if not try_lock(stream):
                        result.append({"slot": slot, "status": "busy"})
                        continue
                    try:
                        path = self.directory / f"{slot}.json"
                        if path.exists():
                            try:
                                stopped = retire(read_json(path))
                            except AnalysisError as error:
                                result.append({"slot": slot, "status": "unverified", "error": str(error)})
                                continue
                            path.unlink()
                            result.append({"slot": slot, "status": "stopped" if stopped else "already_exited"})
                    finally:
                        unlock(stream)
        return result


# Sonar

def compiler_entries(database: Path, root: Path) -> list[dict]:
    """Resolve worktree translation units, object targets and include search paths."""
    result = []
    for entry in _load_compilation_database(database):
        if not isinstance(entry, dict) or not isinstance(entry.get("file"), str):
            raise AnalysisError("Malformed compilation database entry")
        directory = Path(entry.get("directory", str(database.parent))).resolve()
        source = (directory / entry["file"]).resolve()
        if not source.is_relative_to(root):
            continue
        argv = entry.get("arguments") or command_line_arguments(entry.get("command", ""))
        includes = [source.parent]
        output = (directory / entry["output"]).resolve() if isinstance(entry.get("output"), str) else None
        for index, arg in enumerate(argv):
            if arg in {"-o", "/Fo"} and index + 1 < len(argv):
                output = (directory / argv[index + 1]).resolve()
            elif arg.startswith("/Fo") and len(arg) > 3:
                output = (directory / arg[3:]).resolve()
            elif arg.startswith("-o") and len(arg) > 2:
                output = (directory / arg[2:]).resolve()
            if arg in {"-I", "-isystem", "-iquote", "/I"} and index + 1 < len(argv):
                includes.append((directory / argv[index + 1]).resolve())
            elif arg.startswith(("-I", "/I")) and len(arg) > 2:
                includes.append((directory / arg[2:]).resolve())
        if output is None or not output.is_relative_to(database.parent):
            raise AnalysisError(f"No in-build object target for {source.relative_to(root)}")
        result.append({"source": source, "target": output.relative_to(database.parent).as_posix(), "includes": includes})
    return result


def header_context(entries: list[dict], headers: list[Path], root: Path) -> dict[Path, dict]:
    """
    Find a translation unit including each header through its include chain.

    This conservatively traverses literal includes, including conditional branches.
    The subsequent build and IDE sensor evidence validate the chosen context.
    Headers without a provable include chain are never declared clean.
    """
    include_pattern = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.MULTILINE)
    contents = {}
    found = {}
    if any(not path.is_file() for path in headers):
        raise AnalysisError("Selected header is missing")
    for entry in entries:
        if CANCELLED.is_set():
            raise AnalysisError("Header context search cancelled")
        pending = [entry["source"]]
        visited = set()
        while pending:
            path = pending.pop()
            if (path in visited or not path.is_file() or not path.is_relative_to(root)
                    or any(part in {"build", "vendor", "deprecated", "_deps"} for part in path.relative_to(root).parts)):
                continue
            visited.add(path)
            if path in headers:
                found.setdefault(path, entry)
            if path not in contents:
                contents[path] = include_pattern.findall(path.read_text(encoding="utf-8", errors="replace"))
            for include in contents[path]:
                for directory in [path.parent, *entry["includes"]]:
                    candidate = (directory / include).resolve()
                    if candidate.is_file():
                        candidate = original_header(candidate, root)
                        pending.append(candidate)
                        break
        if all(header in found for header in headers):
            return found
    missing = [str(path.relative_to(root)) for path in headers if path not in found]
    raise AnalysisError("No translation unit context for header(s): " + ", ".join(missing))


def original_header(candidate: Path, root: Path) -> Path:
    """Map verified CMake staged public copies to their owning source header."""
    if "target-includes" not in candidate.parts:
        return candidate
    staged = candidate.parts.index("target-includes")
    if candidate.parts[staged + 2:staged + 3] != ("public",):
        return candidate
    original = root / "include" / Path(*candidate.parts[staged + 3:])
    if original.is_file() and digest(original) == digest(candidate):
        return original
    return candidate


def prepare(root: Path, cache: Path, files: list[Path], existing: Path | None,
            timeout: float) -> tuple[Path, dict[Path, dict]]:
    """Let CMake refresh stale inputs and build only selected object targets."""
    if existing is None:
        existing = reusable_database(root, files)
    build = existing.parent.resolve() if existing else cache / "build"
    cmake_cache = build / "CMakeCache.txt"
    if existing:
        if not existing.is_file() or not cmake_cache.is_file():
            raise AnalysisError("Existing compile database requires its CMake build directory")
        home = next((line.split("=", 1)[1] for line in cmake_cache.read_text().splitlines()
                     if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL=")), None)
        if home is None or Path(home).resolve() != root:
            raise AnalysisError("Compilation database belongs to a different worktree")
    command = ["cmake", "-S", str(root), "-B", str(build), "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
    if not existing:
        command.extend(["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug", "-DBUILD_TESTING=ON"])
        if sys.platform == "linux":
            command.extend(["-DHORO_BUILD_RENDER_VULKAN=ON", "-DHORO_ENABLE_IMGUI_UI_TESTS=ON",
                            "-DHORO_ENABLE_OPENTELEMETRY=ON", "-DHORO_BUILD_NETWORK_GNS=ON"])
    configured = quality_run(command, root, timeout)
    if configured.returncode:
        raise AnalysisError("CMake configuration failed; " + configured.stderr[-2000:])
    database = build / "compile_commands.json"
    validate_compile_commands(root, database, files)
    entries = compiler_entries(database, root)
    headers = [path for path in files if path.suffix.lower() not in TRANSLATION_UNIT_SUFFIXES]
    contexts = header_context(entries, headers, root) if headers else {}
    requested = {path for path in files if path.suffix.lower() in TRANSLATION_UNIT_SUFFIXES}
    targets = {entry["target"] for entry in entries if entry["source"] in requested}
    targets.update(entry["target"] for entry in contexts.values())
    if targets:
        # Make exposes object rules in subdirectory Makefiles, Ninja at its root.
        make = "CMAKE_GENERATOR:INTERNAL=Unix Makefiles" in cmake_cache.read_text()
        if make:
            owners = {target.split("CMakeFiles/", 1)[1].split(".dir/", 1)[0] for target in targets}
            commands = [["cmake", "--build", str(build), "--parallel", "2", "--target", *sorted(owners)]]
        else:
            commands = [["cmake", "--build", str(build), "--parallel", "2", "--target", *sorted(targets)]]
        for command in commands:
            built = quality_run(command, root, timeout)
            if built.returncode:
                raise AnalysisError("Compiler context preparation failed; " + (built.stderr or built.stdout)[-2000:])
    return database, contexts


def reusable_database(root: Path, files: list[Path]) -> Path | None:
    """Prefer the canonical skeleton build only after checking worktree identity."""
    database = root / "build/skeleton/compile_commands.json"
    cache = database.parent / "CMakeCache.txt"
    if not database.is_file() or not cache.is_file():
        return None
    home = next((line.split("=", 1)[1] for line in cache.read_text().splitlines()
                 if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL=")), None)
    if home is None or Path(home).resolve() != root:
        return None
    try:
        validate_compile_commands(root, database, files)
    except AnalysisError:
        return None
    return database


def verify_evidence(text: str, files: list[Path]) -> dict:
    """Require file-specific CFamily execution, active rules, and no sensor error."""
    # Only the explicit no-indexed-files retry may precede the completed request.
    # All sensor errors after its last occurrence remain fatal.
    retry = text.rfind("No files were found to be indexed by SonarQube for IDE")
    if retry >= 0:
        next_request = text.find("Analyzing list of", retry)
        if next_request >= 0:
            text = text[next_request:]
    branch_warning = "Couldn't access repository for path" in text
    # JGit in the IDE can fail to discover linked-worktree metadata. This affects
    # cloud branch matching, not CFamily compilation context; retain the warning.
    sensor_text = "\n".join(line for line in text.splitlines()
                            if "Couldn't access repository for path" not in line)
    if "[Error -" in sensor_text or re.search(r"(?i)(failed to analy[sz]e|could not analy[sz]e|not set to a valid|not analyzed)", sensor_text):
        raise AnalysisError("Sonar log reports analysis errors; no clean result can be inferred")
    counts = [int(value) for value in re.findall(r"(\d+) compilation units analyzed", text)]
    rules = [int(value) for value in re.findall(r"(\d+) (?:cpp|c)[,\]]", text)]
    if not counts or max(counts) < 1 or not rules or max(rules) < 1:
        raise AnalysisError("No positive CFamily sensor execution with active C/C++ rules in this request")
    if "Analysis detected" not in text or any(path.as_uri() not in text for path in files):
        raise AnalysisError("Sonar did not confirm completion for all submitted files")
    return {"compilationUnits": max(counts), "activeCppRules": True, "completion": True,
            "branchMatching": "unavailable" if branch_warning else "no_error_reported"}


def binding_evidence(profile: Path, project: str) -> bool:
    """Require the connected server's readiness acknowledgement for this project."""
    pattern = re.compile(r"isReadyForAnalysis\(connectionId: [^,]+, sonarProjectKey: "
                         + re.escape(project) + r", plugins: [^\n]+\) => (true|false)")
    paths = logs(profile)
    if not paths:
        return False
    latest = max(path.relative_to(profile).parts[1] for path in paths)
    text = "\n".join(path.read_text(encoding="utf-8", errors="replace") for path in paths
                     if path.relative_to(profile).parts[1] == latest)
    matches = pattern.findall(text)
    return bool(matches and matches[-1] == "true")


def wait_binding(profile: Path, project: str, timeout: float) -> None:
    """Wait for initial connected-mode synchronization before requesting analysis."""
    deadline = time.monotonic() + timeout
    while not binding_evidence(profile, project):
        if CANCELLED.is_set():
            raise AnalysisError("Connected-mode synchronization cancelled")
        if time.monotonic() >= deadline:
            raise AnalysisError("Connected-mode project synchronization did not complete before timeout")
        time.sleep(0.5)


def request_analysis(root: Path, port: int, files: list[Path], timeout: float, startup: float) -> list[dict]:
    """Keep an in-flight HTTP request cancellable using an owned worker process."""
    worker = ("import json,sys; from pathlib import Path; "
              "sys.path.insert(0,sys.argv[1]); "
              "from analyze import analyze_when_indexed; "
              "print(json.dumps(analyze_when_indexed(int(sys.argv[2]), "
              "[Path(p) for p in json.loads(sys.argv[3])], float(sys.argv[4]), float(sys.argv[5]))))")
    result = quality_run([sys.executable, "-c", worker, str(Path(__file__).parent), str(port),
                  json.dumps([str(path) for path in files]), str(timeout), str(startup)], root, timeout)
    if result.returncode:
        raise AnalysisError("Sonar bridge request failed: " + result.stderr[-1500:])
    findings = json.loads(result.stdout)
    if not isinstance(findings, list) or not all(isinstance(item, dict) for item in findings):
        raise AnalysisError("Sonar worker returned malformed findings")
    return findings


def analyze_files(root: Path, record: dict, contexts: dict, files: list[Path], args) -> tuple[list, list]:
    """Collect file-specific sensor evidence while the caller holds the lease."""
    profile = Path(record["profile"])
    findings, evidence = [], []
    for path in files:
        submitted = [path]
        if path in contexts:
            submitted.insert(0, contexts[path]["source"])
        before = log_offsets(profile)
        result = request_analysis(root, record["port"], submitted, args.timeout, args.startup_timeout)
        deadline = time.monotonic() + 5
        while True:
            try:
                proof = verify_evidence(log_delta(profile, before), submitted)
                break
            except AnalysisError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.1)
        for finding in result:
            reported = finding.get("filePath")
            if isinstance(reported, str) and Path(reported).is_absolute():
                mapped = original_header(Path(reported), root)
                if str(mapped) != reported:
                    finding = {**finding, "filePath": str(mapped), "reportedFilePath": reported}
            findings.append(finding)
        evidence.append({"path": str(path.relative_to(root)), **proof})
    return findings, evidence


def sonar_check(root: Path, cache: Path, repository_cache: Path, files: list[Path], args) -> dict:
    """Hold the session lease through all requests and retain all findings."""
    if not files:
        return {"status": "not_applicable", "findings": []}
    database, contexts = prepare(root, cache, files, args.compile_commands, args.timeout)
    workspace = write_workspace(root, cache, database, args.connection_id, args.project_key)
    pool = SessionPool(repository_cache, root, args.timeout)
    configuration = [str(database), digest(database), args.project_key, args.connection_id]
    with pool.lease() as (slot, previous):
        if previous and previous.get("configuration") != configuration:
            retire(previous)
            previous = {}
        record = pool.open(slot, previous, workspace, args.code_user_data, args.extensions_directory,
                           args.startup_timeout, files[0])
        record["database"] = str(database)
        record["configuration"] = configuration
        write_json(pool.directory / f"{slot}.json", record)
        profile = Path(record["profile"])
        if not record.get("bindingReady"):
            wait_binding(profile, args.project_key, args.timeout)
            record["bindingReady"] = True
            write_json(pool.directory / f"{slot}.json", record)
        findings, evidence = analyze_files(root, record, contexts, files, args)
        if not record.get("bindingReady"):
            raise AnalysisError("Connected-mode project cache is missing; bind/authenticate the managed Sonar profile")
    selected = {str(path) for path in files}
    contextual = [item for item in findings if item.get("filePath") not in selected]
    unique = {json.dumps(item, sort_keys=True): item for item in findings if item.get("filePath") in selected}
    stale = digest(database) != configuration[1]
    return {"status": "incomplete" if stale else ("issues_found" if unique else "clean"),
            "stale": stale, "error": "Compilation database changed during analysis" if stale else None,
            "findings": list(unique.values()),
            "contextFindings": contextual, "evidence": evidence, "compileCommands": str(database),
            "bridge": {"port": record["port"], "pid": record["pid"], "started": record["started"]},
            "versions": [record["extensionVersion"]],
            "extension": {"path": record["extension"], "version": record["extensionVersion"]},
            "backend": "local_sonar_ide", "connectedMode": True,
            "projectKey": args.project_key, "connectionId": args.connection_id,
            "configurationIdentity": configuration,
            "warnings": ["IDE could not match the linked-worktree Git branch; cloud branch parity is unverified"]
            if any(item["branchMatching"] == "unavailable" for item in evidence) else [],
            "submitted": [str(path.relative_to(root)) for path in files]}


# Preflight

#!/usr/bin/env python3























def arguments(argv=None):
    """Keep selectors consistent across both analyzer families."""
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("check", "doctor", "stop"):
        command = commands.add_parser(name)
        command.add_argument("--worktree", type=Path, default=Path.cwd())
        command.add_argument("--format", choices=("text", "json"), default="text")
        command.add_argument("--timeout", type=float, default=600)
        command.add_argument("--project-key", default="horocore_horo-engine")
        command.add_argument("--connection-id", default="horocore")
        command.add_argument("--code-user-data", type=Path, default=default_code_user_data())
        command.add_argument("--extensions-directory", type=Path, default=Path.home() / ".vscode/extensions")
        command.add_argument("--startup-timeout", type=float, default=90)
        if name == "check":
            selectors = command.add_mutually_exclusive_group()
            selectors.add_argument("--base")
            selectors.add_argument("--dirty", action="store_true")
            selectors.add_argument("--files", nargs="+", type=Path)
            command.add_argument("--compile-commands", type=Path)
            command.add_argument("--only", choices=("sonar", "codacy"), help="Intentionally partial provider scope")
        if name == "doctor":
            command.add_argument("--offline", action="store_true", help="Skip cloud configuration comparison")
    args = parser.parse_args(argv)
    if args.timeout <= 0 or args.startup_timeout <= 0:
        parser.error("Timeouts must be positive")
    return args


def selection(root: Path, args) -> tuple[list[Path], list[dict], str, str]:
    """Include committed and local deltas, excluding deleted/outside paths explicitly."""
    head = os.fsdecode(run_git(root, "rev-parse", "HEAD")).strip()
    base = args.base or (None if args.dirty or args.files else "origin/main")
    base_sha = os.fsdecode(run_git(root, "merge-base", "--", validated_git_ref(base), "HEAD")).strip() if base else head
    if args.files:
        requested, source = args.files, "explicit"
    else:
        requested, source = changed_paths(root, base)
    selected, skipped = [], []
    allowed = set(repository_files(root))
    for path in requested:
        absolute = (root / path).resolve()
        if not absolute.is_relative_to(root):
            skipped.append({"path": str(path), "reason": "outside_worktree", "intentional": False})
            continue
        relative = absolute.relative_to(root)
        if not absolute.is_file():
            skipped.append({"path": str(path), "reason": "deleted_or_missing", "intentional": not bool(args.files)})
        elif relative not in allowed:
            skipped.append({"path": str(path), "reason": "excluded_input", "intentional": True})
        elif relative not in selected:
            selected.append(relative)
    return selected, skipped, source, base_sha


def guarded(callback, *args) -> dict:
    """Preserve provider failures alongside successful independent analyses."""
    started = time.monotonic()
    try:
        result = callback(*args)
    except (AnalysisError, OSError, ValueError, KeyError, TypeError) as error:
        result = {"status": "incomplete", "error": str(error)}
    result["durationSeconds"] = round(time.monotonic() - started, 3)
    return result


def tool_versions(root: Path, names: tuple[str, ...]) -> dict:
    """Record executable versions without hiding individual discovery failures."""
    versions = {}
    for tool in names:
        executable = shutil.which(tool)
        try:
            result = quality_run([executable, "--version"], root, 30) if executable else None
            versions[tool] = {"available": bool(result and result.returncode == 0),
                              "version": result.stdout.strip().splitlines()[0]
                              if result and result.stdout.strip() else None}
        except AnalysisError as error:
            versions[tool] = {"available": False, "version": None, "error": str(error)}
    return versions


def annotate(items: list[dict], root: Path, ranges: dict) -> list[dict]:
    """Mark changed-line overlap without hiding other findings or guessing their age."""
    result = []
    normalized_ranges = {Path(path).as_posix(): value for path, value in ranges.items()}
    for item in items:
        item = dict(item)
        path = item.get("filePath")
        if path:
            absolute = Path(path)
            if absolute.is_absolute() and absolute.is_relative_to(root):
                path = absolute.relative_to(root).as_posix()
            else:
                path = absolute.as_posix()
            item["filePath"] = path
        text_range = item.get("textRange", {})
        first = item.get("line", text_range.get("startLine"))
        last = item.get("endLine", text_range.get("endLine", first))
        item["changedLine"] = (any(first <= end and last >= start for start, end in normalized_ranges.get(path or "", []))
                               if isinstance(first, int) and isinstance(last, int) else None)
        result.append(item)
    return sorted(result, key=lambda item: (item["changedLine"] is not True, item.get("filePath", ""),
                                           item.get("line", item.get("textRange", {}).get("startLine", 0))))


def codacy_stage(root: Path, cache: Path, paths: list[Path], timeout: float) -> dict:
    """Use the installed CLI snapshot without updating any repository configuration."""
    if not paths:
        return {"status": "not_applicable", "issues": []}
    config = read_json(root / ".codacy/codacy.config.json")
    mirror = snapshot(root, cache / "mirror")
    info = codacy_info(mirror)
    submitted = [path for path in paths if not excluded(path, config)]
    result = codacy_check(root, mirror, submitted, config, info, timeout)
    cpp = [path for path in submitted if path.suffix.lower() in SUPPORTED_SUFFIXES]
    result["complexity"] = guarded(complexity, root, cpp, result.get("capability", {}), timeout)
    result["duplication"] = guarded(duplication, root, mirror, cpp, config, info, timeout)
    result["excluded"] = [str(path) for path in paths if path not in submitted]
    result["submitted"] = [str(path) for path in submitted]
    if any(result[name]["status"] == "incomplete" for name in ("complexity", "duplication")):
        result["status"] = "incomplete"
    # CLI normally reports relative paths; relocate any absolute mirror paths as well.
    for issue in result.get("issues", []):
        path = Path(issue["filePath"])
        if path.is_absolute() and path.is_relative_to(mirror):
            issue["filePath"] = str(path.relative_to(mirror))
    return result


def preflight_check(root: Path, repository_cache: Path, args) -> dict:
    """Run independent providers concurrently while holding the worktree lease."""
    cache = repository_cache / "worktrees" / identity(root)
    paths, skipped, source, base_sha = selection(root, args)
    before = fingerprint(root)
    ranges = changed_ranges(root, base_sha, paths)
    report = {"schemaVersion": 1, "worktree": str(root), "platform": sys.platform, "head": before["head"], "baseSha": base_sha,
              "selection": source, "files": [str(path) for path in paths], "skipped": skipped,
              "scope": args.only or "sonar_and_codacy", "providers": {},
              "configuration": {str(path): digest(root / path) for path in
                                (Path(".codacy/codacy.config.json"), Path("sonar-project.properties"))
                                if (root / path).is_file()}}
    started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
        pending = {}
        if args.only != "codacy":
            cpp = [root / path for path in paths if path.suffix.lower() in SUPPORTED_SUFFIXES]
            pending["sonarIde"] = executor.submit(guarded, sonar_check, root, cache, repository_cache, cpp, args)
            pending["secrets"] = executor.submit(guarded, secrets, root, paths, args.timeout)
        if args.only != "sonar":
            pending["codacy"] = executor.submit(guarded, codacy_stage, root, cache, paths, args.timeout)
        try:
            report["providers"] = {name: future.result() for name, future in pending.items()}
        except KeyboardInterrupt:
            CANCELLED.set()
            report["providers"] = {name: future.result() for name, future in pending.items()}
            report["cancelled"] = True
    report["stale"] = before != fingerprint(root) or any(
        provider.get("stale") for provider in report["providers"].values())
    for provider in report["providers"].values():
        if "findings" in provider:
            provider["findings"] = annotate(provider["findings"], root, ranges)
        if "issues" in provider:
            provider["issues"] = annotate(provider["issues"], root, ranges)
    report["durationSeconds"] = round(time.monotonic() - started, 3)
    report["versions"] = tool_versions(root, ("sonar", "codacy-analysis", "code", "cmake", "ninja"))
    incomplete = report["stale"] or report.get("cancelled") or any(not item["intentional"] for item in skipped)
    incomplete = incomplete or any(value["status"] == "incomplete" for value in report["providers"].values())
    findings = any(value["status"] == "issues_found" or value.get("issues") or value.get("findings")
                   for value in report["providers"].values())
    report["status"] = "incomplete" if incomplete else ("issues_found" if findings else "clean")
    if not paths and not incomplete:
        report["status"] = "no_changes"
    report["exitCode"] = 2 if incomplete else int(bool(findings))
    return report


def cloud_drift(root: Path, cache: Path, timeout: float) -> dict:
    """Refresh a disposable config for comparison; never import or upload changes."""
    remote = os.fsdecode(run_git(root, "remote", "get-url", "origin")).strip()
    match = re.search(r"github\.com[:/]([^/]+)/([^/]+?)(?:\.git)?$", remote)
    if not match:
        raise AnalysisError("Cannot resolve Codacy GitHub repository from origin")
    mirror = snapshot(root, cache / "mirror")
    target = cache / "cloud-config.json"
    result = quality_run(["codacy-analysis", "init", "--remote", "gh", match[1], match[2],
                  "--config-file", str(target)], mirror, timeout)
    if result.returncode:
        raise AnalysisError(f"Cloud configuration read failed (exit {result.returncode}); verify Codacy authentication")
    local, cloud = read_json(root / ".codacy/codacy.config.json"), read_json(target)

    def comparable(config):
        return {tool["toolId"]: json.dumps({key: value for key, value in tool.items() if key != "patterns"}, sort_keys=True)
                + json.dumps(sorted(tool.get("patterns", []), key=lambda item: item["patternId"]), sort_keys=True)
                for tool in config["tools"]}
    left, right = comparable(local), comparable(cloud)
    changed = sorted(key for key in left.keys() | right.keys() if left.get(key) != right.get(key))
    excludes_changed = set(local.get("exclude", [])) != set(cloud.get("exclude", []))
    return {"status": "drift" if changed or excludes_changed else "matched", "changedTools": changed,
            "excludesChanged": excludes_changed, "localHash": digest(root / ".codacy/codacy.config.json"),
            "cloudHash": digest(target), "snapshot": str(target)}


def doctor(root: Path, cache: Path, args) -> dict:
    """Check local prerequisites and optional Codacy cloud configuration drift."""
    versions = tool_versions(root, ("sonar", "codacy-analysis", "code", "cmake", "ninja"))
    report = {"schemaVersion": 1, "worktree": str(root), "platform": sys.platform, "versions": versions,
              "codacy": guarded(codacy_info, root), "ideUserData": str(args.code_user_data),
              "ideExtensions": str(args.extensions_directory), "coverage": "not_in_scope"}
    if not args.offline:
        report["cloudConfiguration"] = guarded(cloud_drift, root, cache / "doctor", args.timeout)
    else:
        report["cloudConfiguration"] = {"status": "not_checked"}
    report["ideConfiguration"] = {
        "settingsAvailable": (args.code_user_data / "User/settings.json").is_file(),
        "extensionAvailable": bool(list(args.extensions_directory.glob("sonarsource.sonarlint-vscode-*/package.json")))}
    ready = (all(item["available"] for item in versions.values())
             and all(report["ideConfiguration"].values()) and report["codacy"].get("status") != "incomplete")
    report["status"] = "ready_for_local_check" if ready else "incomplete"
    report["exitCode"] = 2 if report["status"] == "incomplete" else 0
    return report


def print_provider(name: str, result: dict) -> None:
    """Print provider findings, omissions and advisory metrics."""
    print(f"  {name}: {result['status']} ({result.get('durationSeconds', 0)}s)")
    if result.get("error"):
        print(f"    {result['error']}")
    for error in result.get("errors", []) + result.get("unsupported", []):
        print(f"    incomplete: {error}")
    for unavailable in result.get("capability", {}).get("unavailable", []):
        print(f"    unavailable: {unavailable}")
    for warning in result.get("warnings", []):
        print(f"    warning: {warning}")
    for item in result.get("issues", []) + result.get("findings", []):
        line = item.get("line", item.get("textRange", {}).get("startLine", "?"))
        marker = "changed" if item.get("changedLine") else "context"
        print(f"    [{marker}] {item.get('severity', 'unknown')} {item.get('patternId', item.get('ruleKey', ''))} "
              f"{item.get('filePath', '?')}:{line}: {item.get('message', '')}")
    for metric in ("complexity", "duplication"):
        if metric in result:
            value = result[metric]
            print(f"    {metric} (advisory): {value['status']} {value.get('error', '')}")
            for item in value.get("files", []):
                print(f"      {item['path']}: CCN {item['cyclomaticComplexity']}")
            if value.get("clones"):
                print(f"      {len(value['clones'])} clone groups touch selected files; see JSON for occurrences")
    for item in result.get("skipped", []):
        print(f"    skipped: {item}")


def output(report: dict, path: Path, format_name: str) -> None:
    """Keep the raw report and print a concise, actionable local summary."""
    report["reportPath"] = str(path)
    write_json(path, report)
    if format_name == "json":
        print(json.dumps(report, ensure_ascii=True, indent=2))
        return
    print(f"Quality preflight: {report['status']} | {report['worktree']}")
    if "versions" in report:
        for name, version in report["versions"].items():
            print(f"  {name}: {version['version'] if version['available'] else 'unavailable'}")
        for name in ("cloudConfiguration",):
            if name not in report:
                continue
            value = report.get(name, {})
            print(f"  {name}: {value.get('status')}; {value.get('error') or value.get('changedTools', '')}")
    for name, result in report.get("providers", {}).items():
        print_provider(name, result)
    if report.get("stale"):
        print("  Inputs changed during analysis; rerun this scope.")
    for skipped in report.get("skipped", []):
        print(f"  skipped: {skipped}")
    print(f"Report: {path}")


def preflight_main(argv=None) -> int:
    """Never claim a clean result when a provider or prerequisite is missing."""
    args = arguments(argv)
    CANCELLED.clear()
    try:
        ensure_supported()
        root = Path(os.fsdecode(run_git(args.worktree, "rev-parse", "--show-toplevel")).strip()).resolve()
        cache = cache_directory(root)
        args.code_user_data = args.code_user_data.resolve()
        args.extensions_directory = args.extensions_directory.resolve()
        if getattr(args, "compile_commands", None):
            args.compile_commands = (root / args.compile_commands).resolve()
        with lock(cache / "worktrees" / identity(root) / "check.lock", args.timeout):
            if args.command == "check":
                report = preflight_check(root, cache, args)
            elif args.command == "doctor":
                report = doctor(root, cache, args)
            else:
                stopped = SessionPool(cache, root, args.timeout).stop()
                busy = any(item["status"] in {"busy", "unverified"} for item in stopped)
                report = {"worktree": str(root), "status": "busy" if busy else "stopped", "sessions": stopped,
                          "exitCode": 2 if busy else 0}
        target = cache / "worktrees" / identity(root) / "reports" / f"{time.time_ns()}-{uuid.uuid4().hex[:8]}.json"
        output(report, target, args.format)
        return report["exitCode"]
    except (AnalysisError, OSError, ValueError, KeyboardInterrupt) as error:
        print(json.dumps({"status": "incomplete", "exitCode": 2, "error": str(error)}, ensure_ascii=True))
        return 2


# Complexity

#!/usr/bin/env python3












def complexity_main(argv=None) -> int:
    """Accept explicit C/C++ paths, a branch delta, or the dirty worktree."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--files", nargs="+", type=Path, default=[])
    parser.add_argument("--base")
    args = parser.parse_args(argv)
    if args.files and args.base:
        parser.error("Explicit files cannot be combined with --base")
    try:
        root = Path(run_git(Path.cwd(), "rev-parse", "--show-toplevel").decode().strip()).resolve()
        requested = args.files or changed_paths(root, args.base)[0]
        selected = []
        for path in requested:
            absolute = (root / path).resolve()
            if absolute.is_file() and absolute.is_relative_to(root) and absolute.suffix.lower() in SUPPORTED_SUFFIXES:
                selected.append(absolute.relative_to(root))
        capability, code = json_command(["codacy-analysis", "analyze", "--inspect", "--tool", "Lizard",
                                         "--output-format", "json", "--no-log", "--no-update-notifier"], root)
        if code or capability.get("errors") or capability.get("capability", {}).get("unavailable"):
            raise AnalysisError("Codacy could not resolve Lizard")
        print(json.dumps(complexity(root, sorted(set(selected)), capability["capability"], 600), indent=2))
        return 0
    except (AnalysisError, OSError, ValueError) as error:
        print(json.dumps({"status": "incomplete", "error": str(error)}))
        return 2


# Coverage

#!/usr/bin/env python3










def repository_path(path: Path, root: Path) -> Path:
    """Reject resolved input/output paths outside the selected repository."""
    resolved = os.path.realpath(path)
    base = os.path.realpath(root)
    if resolved != base and not resolved.startswith(base.rstrip(os.sep) + os.sep):
        raise ValueError(f"Path outside repository: {path.name}")
    return Path(resolved)


def invocation_path(path: Path) -> str:
    """Validate CLI filesystem access against the trusted invocation directory."""
    resolved = os.path.realpath(path)
    base_dir = os.path.realpath(os.getcwd())
    prefix = base_dir if base_dir.endswith(os.sep) else base_dir + os.sep
    if resolved != base_dir and not resolved.startswith(prefix):
        raise ValueError("Path outside invocation directory")
    return resolved


def read_report(path: Path, root: Path) -> dict[tuple[str, int], bool]:
    """Normalize source paths and retain every measured line, including misses."""
    from defusedxml import ElementTree as element_tree
    from defusedxml.common import DefusedXmlException
    document = element_tree.parse(repository_path(path, root)).getroot()
    if document.tag != "coverage" or document.get("version") != "1":
        raise ValueError(f"Unsupported coverage format: {path.name}")
    lines = {}
    for source in document.findall("file"):
        source_path = Path(source.attrib["path"])
        if not source_path.is_absolute():
            source_path = root / source_path
        normalized = source_path.resolve().relative_to(root.resolve()).as_posix()
        for entry in source.findall("lineToCover"):
            number = int(entry.attrib["lineNumber"])
            covered = entry.attrib["covered"]
            if number < 1 or covered not in {"true", "false"}:
                raise ValueError(f"Invalid coverage line in {path.name}")
            key = (normalized, number)
            # gcovr emits repeated lines for multiple template instantiations.
            # A source line is covered if any instance executed.
            lines[key] = lines.get(key, False) or covered == "true"
    if not lines:
        raise ValueError(f"Empty coverage report: {path.name}")
    return lines


def describe_lines(keys: set[tuple[str, int]]) -> list[dict]:
    """Keep complete differences in the artifact, ordered for review."""
    return [{"file": file, "line": line} for file, line in sorted(keys)]


def compare_reports(baseline: dict, candidate: dict) -> dict:
    """Compare line identities and covered flags, rather than just percentages."""
    baseline_keys = set(baseline)
    candidate_keys = set(candidate)
    missing = baseline_keys - candidate_keys
    extra = candidate_keys - baseline_keys
    changed = {
        key for key in baseline_keys & candidate_keys
        if baseline[key] != candidate[key]
    }
    return {
        "equivalent": not (missing or extra or changed),
        "gcovr": {"lines": len(baseline), "covered": sum(baseline.values())},
        "fastcov": {"lines": len(candidate), "covered": sum(candidate.values())},
        "missingLines": describe_lines(missing),
        "extraLines": describe_lines(extra),
        "changedLines": [
            {"file": file, "line": line,
             "gcovrCovered": baseline[(file, line)],
             "fastcovCovered": candidate[(file, line)]}
            for file, line in sorted(changed)
        ],
    }


def format_summary(result: dict) -> str:
    """Explain timing and equivalence separately in the workflow summary."""
    timings = result["seconds"]
    text = [
        "## Fastcov coverage experiment", "",
        "Sonar continues to use the gcovr baseline report.", "",
        "| Collector | Seconds | Measured lines | Covered lines |",
        "| --- | ---: | ---: | ---: |",
    ]
    for name in ("gcovr", "fastcov"):
        metrics = result[name]
        text.append(
            f"| {name} | {timings[name]} | {metrics['lines']} | {metrics['covered']} |"
        )
    text.extend([
        "", f"Line coverage equivalent: **{result['equivalent']}**",
        f"Missing lines: {len(result['missingLines'])}; "
        f"extra lines: {len(result['extraLines'])}; "
        f"changed covered flags: {len(result['changedLines'])}.",
        "", "Fastcov time includes JSON collection and Sonar XML conversion.",
        "This comparison checks line coverage; it does not establish branch parity.",
        "See the coverage experiment artifact for complete differences.", "",
    ])
    return "\n".join(text)


def coverage_main(argv=None) -> int:
    """Write evidence; coverage differences remain diagnostic during the trial."""
    from defusedxml import ElementTree as element_tree
    from defusedxml.common import DefusedXmlException
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--gcovr-report", type=Path, required=True)
    parser.add_argument("--fastcov-report", type=Path, required=True)
    parser.add_argument("--gcovr-seconds", type=int, required=True)
    parser.add_argument("--fastcov-seconds", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.root.resolve() != Path.cwd().resolve():
            raise ValueError("Repository root must be the current working directory")
        root = Path.cwd().resolve()
        output = invocation_path(args.output)
        result = compare_reports(
            read_report(args.gcovr_report, root),
            read_report(args.fastcov_report, root),
        )
        result["seconds"] = {
            "gcovr": args.gcovr_seconds, "fastcov": args.fastcov_seconds,
        }
        with open(output, "w", encoding="utf-8") as stream:
            stream.write(json.dumps(result, indent=2) + "\n")
        print(format_summary(result))
    except (OSError, element_tree.ParseError, DefusedXmlException, ValueError, KeyError) as error:
        parser.exit(2, f"Coverage comparison failed: {error}\n")
    print(f"Line coverage equivalent: {result['equivalent']}")
    return 0


# Fastcov

#!/usr/bin/env python3











def noncode(text: str) -> bool:
    """Match gcovr's zero-hit comment, brace and standalone else heuristic."""
    text = re.sub(r"//.*$", "", text)
    text = re.sub(r"/\*.*?\*/", "", text)
    text = re.sub(r"\s+", "", text)
    return text in {"", "{", "}", "else"}


def source_counts(tests: dict) -> dict[int, int]:
    """Merge every test and template counter without converting negative hits to coverage."""
    if not isinstance(tests, dict):
        raise ValueError("Invalid fastcov test map")
    result = {}
    for test in tests.values():
        if not isinstance(test, dict) or not isinstance(test.get("lines"), dict):
            raise ValueError("Invalid fastcov line map")
        for line, count in test["lines"].items():
            number = int(line)
            if number < 1 or not isinstance(count, int) or isinstance(count, bool) or count < 0:
                raise ValueError("Invalid fastcov line counter")
            result[number] = result.get(number, 0) + count
    return result


def convert(data: dict, root: Path) -> tuple:
    """Require real repository sources and emit one boolean per measured source line."""
    from defusedxml import ElementTree as element_tree
    # DefusedXML exposes a safe parser and serializer; the template is a constant.
    document = element_tree.fromstring('<coverage version="1"/>')
    measured = covered = excluded = 0
    if not isinstance(data, dict):
        raise ValueError("Invalid fastcov report")
    sources = data["sources"]
    if not isinstance(sources, dict) or not sources:
        raise ValueError("Empty fastcov report")
    for name, tests in sorted(sources.items()):
        path = repository_path(Path(name), root)
        text = path.read_text(encoding="utf-8").splitlines()
        counts = source_counts(tests)
        file = document.makeelement("file", {"path": path.relative_to(root).as_posix()})
        document.append(file)
        for number, count in sorted(counts.items()):
            if number > len(text):
                raise ValueError(f"Coverage line exceeds source length: {path.name}:{number}")
            if count == 0 and noncode(text[number - 1]):
                excluded += 1
                continue
            file.append(file.makeelement("lineToCover", {
                "lineNumber": str(number), "covered": str(count > 0).lower(),
            }))
            measured += 1
            covered += count > 0
    if measured == 0:
        raise ValueError("No measured source lines")
    return document, {"lines": measured, "covered": covered, "excludedNoncode": excluded}


def fastcov_main(argv=None) -> int:
    """Validate repository paths before reading counters or writing the Sonar report."""
    from defusedxml import ElementTree as element_tree
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    root = Path.cwd().resolve()
    try:
        source = repository_path(args.input, root)
        output = repository_path(args.output, root)
        document, metrics = convert(json.loads(source.read_text(encoding="utf-8")), root)
        output.write_bytes(element_tree.tostring(document, encoding="utf-8", xml_declaration=True))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(2, f"Fastcov conversion failed: {error}\n")
    print(json.dumps(metrics, sort_keys=True))
    return 0


# Collector

#!/usr/bin/env python3









BATCH_FILES = 64


def area(root: Path, build: Path, stage: Path) -> tuple:
    """Require an existing, dedicated staging directory inside the owned build."""
    if stage.is_symlink():
        raise ValueError("Coverage staging directory cannot be a symlink")
    root = root.resolve(strict=True)
    build = build.resolve(strict=True)
    stage = stage.resolve(strict=True)
    if not build.is_relative_to(root) or build == root:
        raise ValueError("Coverage build must be inside the repository, not its root")
    if stage.parent != build or not stage.name.startswith("coverage-batches.") or not stage.is_dir():
        raise ValueError("Coverage staging directory must be a dedicated build child")
    return root, build, stage


def nul_manifest(output: Path, paths: list[Path]) -> None:
    """Preserve filesystem names without whitespace, newline or Unicode splitting."""
    output.write_bytes(b"".join(os.fsencode(path) + b"\0" for path in paths))


def plan(root: Path, build: Path, stage: Path) -> dict:
    """Assign every sorted gcno exactly once to bounded, data-only manifests."""
    _, build, stage = area(root, build, stage)
    files = sorted(build.rglob("*.gcno"))
    if not files:
        raise ValueError("No gcno inputs; refusing to publish empty coverage")
    manifests = []
    for offset in range(0, len(files), BATCH_FILES):
        manifest = stage / f"batch-{len(manifests):06d}.files"
        nul_manifest(manifest, files[offset:offset + BATCH_FILES])
        manifests.append(manifest)
    nul_manifest(stage / "batches.manifest", manifests)
    return {"inputs": len(files), "batches": len(manifests), "batchLimit": BATCH_FILES}


def publish(root: Path, build: Path, stage: Path) -> dict:
    """Validate merged data before individually atomic, nontransactional replaces."""
    root, build, stage = area(root, build, stage)
    merged = stage / "fastcov-coverage.json"
    document, metrics = convert(json.loads(merged.read_text(encoding="utf-8")), root)
    xml = stage / "coverage.xml"
    xml.write_bytes(element_tree.tostring(document, encoding="utf-8", xml_declaration=True))
    # Batch/merge/conversion failures touch neither authoritative output. A
    # publication I/O failure can replace one report: the pair is not a transaction.
    # The CLI fails terminally and the CI host's success-only upload gate stays closed.
    os.replace(merged, build / "fastcov-coverage.json")
    os.replace(xml, build / "coverage.xml")
    return metrics


def collector_main(argv=None) -> int:
    """Expose only planning/publication data capabilities, not executable inputs."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("plan", "publish"))
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--build-directory", type=Path, required=True)
    parser.add_argument("--stage", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        operation = plan if args.operation == "plan" else publish
        metrics = operation(args.root, args.build_directory, args.stage)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(2, f"Coverage data operation failed: {error}\n")
    print(json.dumps(metrics, sort_keys=True))
    return 0


TOOL_COMMANDS = {'complexity': complexity_main, 'coverage-compare': coverage_main, 'coverage-convert': fastcov_main, 'coverage': collector_main, 'check': preflight_main, 'doctor': preflight_main, 'stop': preflight_main}


def main(argv=None) -> int:
    """Select an explicit domain command; all implementation lives in this file."""
    argv = list(sys.argv[1:] if argv is None else argv)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=tuple(TOOL_COMMANDS))
    if not argv or argv[0] not in TOOL_COMMANDS:
        parser.parse_args(argv)
    command = argv[0]
    return TOOL_COMMANDS[command](argv if command in {"check", "doctor", "stop"} else argv[1:]) or 0


if __name__ == "__main__":
    raise SystemExit(main())
