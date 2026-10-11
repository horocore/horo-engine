"""Own developer subprocess trees and finish cleanup before releasing build leases."""

from __future__ import annotations

from dataclasses import dataclass
import errno
import os
from pathlib import Path
import shutil
import signal
# Required stdlib for explicit, shell-free owned process execution.
import subprocess  # nosec B404
import time
from typing import Mapping, Sequence


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


def _windows_run(command: Sequence[str], cwd: Path, env: Mapping[str, str] | None) -> int:
    """Preserve ordinary Windows execution; interrupted tree cleanup is unqualified."""
    # Caller-authorized executable and argv are validated by run; no command shell parses arguments.
    # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit
    with subprocess.Popen(command, cwd=cwd, env=env, shell=False,  # nosec B603
                          creationflags=subprocess.CREATE_NEW_PROCESS_GROUP) as process:
        try:
            return process.wait()
        except KeyboardInterrupt as error:
            try:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)
            except (Exception, KeyboardInterrupt, SystemExit) as cleanup_error:
                raise CleanupError("Windows interrupted process cleanup was not confirmed") from cleanup_error
            raise CleanupError("Windows interruption cleanup cannot verify descendant termination") from error


def run(command: Sequence[str], cwd: Path, env: Mapping[str, str] | None = None) -> int:
    """Run argv without a shell; return 130 only after verified POSIX tree cleanup."""
    command = validated_command(command, cwd, env)
    if os.name == "nt":
        return _windows_run(command, cwd, env)
    # Caller-authorized absolute executable and validated argv; a shell never interprets arguments.
    # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit
    process = subprocess.Popen(command, cwd=cwd, env=env, start_new_session=True, shell=False)  # nosec B603
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
        except (Exception, KeyboardInterrupt, SystemExit) as error:
            raise CleanupError("Interrupted developer subprocess cleanup was not confirmed") from error
        return 130
    except CleanupError:
        raise
    except BaseException:
        # Inventory/launch errors also must not silently release an active owned root.
        try:
            _cleanup(process, known)
        except (Exception, KeyboardInterrupt, SystemExit) as error:
            raise CleanupError("Developer subprocess cleanup after failure was not confirmed") from error
        raise
