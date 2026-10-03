"""Shared process, Git, and cache primitives for local quality checks."""

from __future__ import annotations

import contextlib
import hashlib
import json
import os
import signal
import subprocess
import threading
import time
from pathlib import Path

from sonar_ide_analysis import AnalysisError, paths_from_nul_output, run_git

CANCELLED = threading.Event()


def run(argv: list[str], root: Path, timeout: float = 600, env: dict | None = None) -> subprocess.CompletedProcess[str]:
    """Bound an owned process group, including descendants on cancellation."""
    with subprocess.Popen(argv, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE,  # nosec B603
                          # Explicit argv, shell disabled; commands are authorized local tooling.
                          text=True, start_new_session=True, env=env) as process:
        try:
            deadline = time.monotonic() + timeout
            while True:
                if CANCELLED.is_set() or time.monotonic() >= deadline:
                    raise subprocess.TimeoutExpired(argv, timeout)
                try:
                    stdout, stderr = process.communicate(timeout=max(0.01, min(0.5, deadline - time.monotonic())))
                    break
                except subprocess.TimeoutExpired:
                    continue
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.communicate()
            raise AnalysisError(f"Cancelled or timed out: {Path(argv[0]).name}") from None
    return subprocess.CompletedProcess(argv, process.returncode, stdout, stderr)


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
    base = Path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache")))
    result = base.resolve() / "horo-quality" / identity(common)
    if result.is_relative_to(root):
        raise AnalysisError("Quality cache must be outside the source tree")
    result.mkdir(parents=True, exist_ok=True, mode=0o700)
    return result


@contextlib.contextmanager
def lock(path: Path, timeout: float = 600):
    """Release Linux advisory locks on every exit, including process failure."""
    import fcntl

    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    with path.open("a", encoding="utf-8") as stream:
        deadline = time.monotonic() + timeout
        while True:
            if CANCELLED.is_set():
                raise AnalysisError("Quality lease acquisition cancelled")
            try:
                fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise AnalysisError(f"Timed out waiting for quality lease: {path.name}") from None
                time.sleep(0.1)
        try:
            yield
        finally:
            fcntl.flock(stream, fcntl.LOCK_UN)


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
