"""Local main-derived compiler checkpoints with leased, atomic A/B publication."""

from __future__ import annotations

import contextlib
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import time
from typing import Callable, Iterator, Mapping
import uuid

_GENERATION = re.compile(r"gen-[0-9a-f]{32}")
_COMMIT = re.compile(r"[0-9a-f]{40}(?:[0-9a-f]{24})?")
_OWNER = "horo-local-build-cache-v1"
_MAX_SIZE = re.compile(r"[1-9][0-9]*(?:[KMGT]i?B?|[kmgt])?")


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
    result = subprocess.run(["git", "-c", "core.fsmonitor=false", "-C", str(repo), *arguments], check=False,
                            capture_output=True, text=True)
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
        if (root / "cleanup-unverified.json").exists():
            raise CacheError(f"An earlier build has unverified descendants; inspect {root / 'cleanup-unverified.json'} before further builds")
        yield


def record_cleanup_failure(repo: Path) -> None:
    """Preserve generations and block new builds until orphan ownership is audited."""
    root = cache_directory(repo)
    _initialize(root, repo)
    _atomic_json(root / "cleanup-unverified.json", {"schema": 1, "reason": "subprocess descendant termination was not confirmed"})


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
                                         directory / "readonly.conf")):
        return None
    marker = _read_json(directory / "generation.json")
    if not marker or marker.get("schema") != 1 or marker.get("owner") != _OWNER or marker.get("state") != "ready":
        return None
    if marker.get("main_commit") != commit or marker.get("generation") != name:
        return None
    if not (directory / "compiler").is_dir() or not (directory / "readonly.conf").is_file():
        return None
    size = marker.get("max_size")
    if not isinstance(size, str) or not _MAX_SIZE.fullmatch(size):
        return None
    try:
        if (directory / "readonly.conf").read_text(encoding="utf-8") != _config(size, readonly=True):
            return None
    except (OSError, UnicodeError):
        return None
    return directory, pointer


def _binding(directory: Path, commit: str, source: Path, launcher: str,
             environ: Mapping[str, str] | None, *, readonly: bool) -> CacheBinding:
    environment = {key: value for key, value in (os.environ if environ is None else environ).items()
                   if not key.upper().startswith("CCACHE_")}
    environment.update({"CCACHE_DIR": str(directory / "compiler"),
                        "CCACHE_CONFIGPATH": str(directory / ("readonly.conf" if readonly else "writer.conf")),
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
                                 "cleanup_blocked": (root / "cleanup-unverified.json").exists()}
    if ready:
        result.update(ready[1])
    return result


def _config(max_size: str, *, readonly: bool) -> str:
    # ccache accepts multiline remote_storage values; an empty value must be last.
    return (f"compiler_check = content\nsloppiness =\nmax_size = {max_size}\n"
            f"read_only = {'true' if readonly else 'false'}\nremote_storage =\n")


def _write_configs(directory: Path, max_size: str) -> None:
    for name, readonly in (("writer.conf", False), ("readonly.conf", True)):
        (directory / name).write_text(_config(max_size, readonly=readonly), encoding="utf-8")


def _seed(source: Path, destination: Path) -> None:
    """Copy compiler entries without aliases or links to the previous generation."""
    if any(path.is_symlink() for path in source.rglob("*")):
        raise CacheError("Previous compiler generation contains symlinks; refusing to copy")
    shutil.copytree(source, destination, dirs_exist_ok=True, copy_function=shutil.copy2)


def _retire(root: Path, active: str) -> None:
    """Delete only ready manager-owned generations whose readers have all released."""
    if any(path.is_symlink() for path in (root, *root.parents, root / "generations", root / "leases")):
        raise CacheError("Refusing retirement through a symlinked cache directory")
    if (root / "cleanup-unverified.json").exists():
        return
    for directory in (root / "generations").iterdir():
        if directory.name == active or not _GENERATION.fullmatch(directory.name) or directory.is_symlink():
            continue
        marker = _read_json(directory / "generation.json")
        if not marker or marker.get("schema") != 1 or marker.get("owner") != _OWNER or marker.get("state") != "ready":
            continue
        if marker.get("generation") != directory.name:
            continue
        # Failed/partial worktrees must stay registered and recoverable.
        if (directory / "source").exists() or (directory / "source").is_symlink():
            continue
        try:
            with _FileLock(root / "leases" / f"{directory.name}.lock", blocking=False):
                if (root / "cleanup-unverified.json").exists():
                    return
                if directory.resolve().parent != (root / "generations").resolve():
                    raise CacheError("Refusing generation retirement outside managed storage")
                shutil.rmtree(directory)
        except (BlockingIOError, OSError):
            continue


def _new_candidate(root: Path, commit: str, max_size: str) -> tuple[Path, dict[str, object]]:
    """Create an unpublished, manager-owned generation before seeding or checkout creation."""
    name = f"gen-{uuid.uuid4().hex}"
    directory = root / "generations" / name
    directory.mkdir()
    (root / "leases" / f"{name}.lock").touch()
    marker: dict[str, object] = {"schema": 1, "owner": _OWNER, "generation": name,
                                 "main_commit": commit, "max_size": max_size, "state": "preparing"}
    _atomic_json(directory / "generation.json", marker)
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
        if (root / "cleanup-unverified.json").exists():
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
            _atomic_json(directory / "generation.json", marker)
            return code
        if _git(source, "rev-parse", "HEAD") != commit or _git(source, "status", "--porcelain"):
            marker["state"] = "failed"
            _atomic_json(directory / "generation.json", marker)
            raise CacheError("Cache producer modified pinned main; checkpoint was not published")
        _git(repo, "worktree", "remove", str(source))
        marker["state"] = "ready"
        _atomic_json(directory / "generation.json", marker)
        _atomic_json(root / "ready.json", {"schema": 1, "generation": name, "main_commit": commit})
        _retire(root, name)
        return 0
