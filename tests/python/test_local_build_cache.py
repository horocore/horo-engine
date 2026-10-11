"""Regression tests for local cache publication, source ownership and reader leases."""

from __future__ import annotations

import importlib.util
from contextlib import contextmanager
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("horo_local_build_cache", ROOT / "scripts" / "local_build_cache.py")
assert SPEC and SPEC.loader
cache = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = cache
SPEC.loader.exec_module(cache)


def git(repo: Path, *arguments: str) -> str:
    return subprocess.run(["git", "-C", str(repo), *arguments], check=True,
                          capture_output=True, text=True).stdout.strip()


@pytest.fixture
def repository(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> tuple[Path, Path, str]:
    repo = tmp_path / "checkout with spaces türkçe"
    repo.mkdir()
    git(repo, "init", "-b", "main")
    git(repo, "config", "user.name", "Cache Regression")
    git(repo, "config", "user.email", "cache@example.invalid")
    (repo / "input.txt").write_text("main input\n", encoding="utf-8")
    (repo / ".gitignore").write_text("/build/\n", encoding="utf-8")
    git(repo, "add", "input.txt", ".gitignore")
    git(repo, "commit", "-m", "fixture")
    commit = git(repo, "rev-parse", "HEAD")
    git(repo, "update-ref", "refs/remotes/origin/main", commit)
    root = tmp_path / "cache with spaces türkçe"
    monkeypatch.setattr(cache, "cache_directory", lambda _: root)
    original_which = shutil.which
    monkeypatch.setattr(cache.shutil, "which", lambda name: "/tools/ccache" if name == "ccache" else original_which(name))
    return repo, root, commit


def successful_build(source: Path, build: Path, binding: cache.CacheBinding) -> int:
    build.mkdir(parents=True)
    (build / "artifact.o").touch()
    (Path(binding.env["CCACHE_DIR"]) / "entry").write_text("object", encoding="utf-8")
    return 0


def test_discovery_miss_does_not_create_storage(repository: tuple[Path, Path, str]) -> None:
    repo, root, _ = repository
    with cache.automatic_cache(repo) as binding:
        assert binding is None
    assert cache.status(repo)["ready"] is False
    assert not root.exists()


def test_unverified_cleanup_blocks_new_builds_and_retirement(repository: tuple[Path, Path, str]) -> None:
    repo, root, _ = repository
    assert cache.prepare_cache(repo, successful_build) == 0
    old = root / "generations" / cache.status(repo)["generation"]
    cache.record_cleanup_failure(repo)
    assert cache.status(repo)["cleanup_blocked"] is True
    with pytest.raises(cache.CacheError, match="unverified descendants"):
        with cache.heavy_build_lock(repo):
            pytest.fail("an unverified earlier build must block new compilation")
    with pytest.raises(cache.CacheError, match="unverified descendants"):
        cache.prepare_cache(repo, successful_build)
    cache._retire(root, "different-generation")
    assert old.exists()


def test_prepare_uses_clean_pinned_main_without_touching_feature_checkout(
    repository: tuple[Path, Path, str],
) -> None:
    repo, root, commit = repository
    git(repo, "checkout", "-b", "feature")
    (repo / "input.txt").write_text("user changes\n", encoding="utf-8")
    before = git(repo, "status", "--porcelain")

    def inspect(source: Path, build: Path, binding: cache.CacheBinding) -> int:
        assert source != repo and source.is_relative_to(root)
        assert build == source / "build" / "dev-cache"
        assert git(source, "rev-parse", "HEAD") == commit
        assert subprocess.run(["git", "-C", str(source), "symbolic-ref", "-q", "HEAD"],
                              capture_output=True).returncode == 1
        assert git(source, "status", "--porcelain") == ""
        assert (source / "input.txt").read_text() == "main input\n"
        assert binding.main_commit == commit
        return successful_build(source, build, binding)

    assert cache.prepare_cache(repo, inspect) == 0
    assert git(repo, "branch", "--show-current") == "feature"
    assert git(repo, "status", "--porcelain") == before
    assert (repo / "input.txt").read_text() == "user changes\n"
    pointer = cache.status(repo)
    assert pointer["ready"] is True and pointer["main_commit"] == commit
    generation = root / "generations" / pointer["generation"]
    assert not (generation / "source").exists()
    assert not (generation / "source" / "build").exists()
    assert len(git(repo, "worktree", "list", "--porcelain").split("worktree ")) == 2


def test_failure_keeps_ready_generation_and_preserves_failed_producer(
    repository: tuple[Path, Path, str],
) -> None:
    repo, root, _ = repository
    assert cache.prepare_cache(repo, successful_build) == 0
    before = (root / "ready.json").read_bytes()
    assert cache.prepare_cache(repo, lambda *_: 7) == 7
    assert (root / "ready.json").read_bytes() == before
    failed = [path for path in (root / "generations").iterdir()
              if json.loads((path / "generation.json").read_text())["state"] == "failed"]
    assert len(failed) == 1 and (failed[0] / "source").is_dir()


def test_success_switches_generations_and_seed_is_independent(
    repository: tuple[Path, Path, str],
) -> None:
    repo, root, _ = repository
    assert cache.prepare_cache(repo, successful_build) == 0
    old = root / "generations" / cache.status(repo)["generation"]

    def inspect(source: Path, build: Path, binding: cache.CacheBinding) -> int:
        entry = Path(binding.env["CCACHE_DIR"]) / "entry"
        assert entry.read_text() == "object"
        entry.write_text("replacement", encoding="utf-8")
        assert (old / "compiler" / "entry").read_text() == "object"
        return 0

    assert cache.prepare_cache(repo, inspect) == 0
    assert cache.status(repo)["generation"] != old.name
    assert not old.exists()


def test_in_use_generation_survives_retirement(repository: tuple[Path, Path, str]) -> None:
    repo, root, _ = repository
    assert cache.prepare_cache(repo, successful_build) == 0
    with cache.automatic_cache(repo) as binding:
        assert binding is not None
        old = root / "generations" / binding.generation
        assert cache.prepare_cache(repo, successful_build) == 0
        assert old.is_dir()
        assert (old / "compiler" / "entry").read_text() == "object"
    assert cache.prepare_cache(repo, successful_build) == 0
    assert not old.exists()


def test_consumer_configuration_discards_inherited_ccache_overrides(
    repository: tuple[Path, Path, str],
) -> None:
    repo, root, _ = repository
    assert cache.prepare_cache(repo, successful_build) == 0
    inherited = {"PATH": "/tools", "KEEP": "value", "CCACHE_SLOPPINESS": "time_macros",
                 "CCACHE_READONLY": "false", "CCACHE_REMOTE_STORAGE": "unsafe",
                 "CCACHE_CONFIGPATH": "/outside/config"}
    with cache.automatic_cache(repo, inherited) as binding:
        assert binding is not None
        assert binding.env["KEEP"] == "value"
        assert not {"CCACHE_SLOPPINESS", "CCACHE_READONLY", "CCACHE_REMOTE_STORAGE"} & binding.env.keys()
        assert binding.env["CCACHE_BASEDIR"] == str(repo.resolve())
        config = Path(binding.env["CCACHE_CONFIGPATH"])
        assert config.is_relative_to(root)
        assert "compiler_check = content" in config.read_text()
        assert "sloppiness =\n" in config.read_text()
        assert "read_only = true" in config.read_text()
    assert inherited["CCACHE_SLOPPINESS"] == "time_macros"


@pytest.mark.parametrize("pointer", [None, [], {"schema": 1, "generation": "../../outside", "main_commit": "a" * 40},
                                      {"schema": 1, "generation": "gen-" + "a" * 32, "main_commit": "invalid"}])
def test_malformed_pointer_is_safe_miss(repository: tuple[Path, Path, str], pointer: object) -> None:
    repo, root, _ = repository
    root.mkdir()
    (root / "ready.json").write_text("broken json" if pointer is None else json.dumps(pointer))
    before = (root / "ready.json").read_bytes()
    with cache.automatic_cache(repo) as binding:
        assert binding is None
    assert (root / "ready.json").read_bytes() == before


def test_main_mutation_prevents_publication(repository: tuple[Path, Path, str]) -> None:
    repo, root, _ = repository
    assert cache.prepare_cache(repo, successful_build) == 0
    before = (root / "ready.json").read_bytes()

    def dirty(source: Path, *_: object) -> int:
        (source / "input.txt").write_text("changed during compile")
        return 0

    with pytest.raises(cache.CacheError, match="modified pinned main"):
        cache.prepare_cache(repo, dirty)
    assert (root / "ready.json").read_bytes() == before


def test_missing_ccache_preserves_ordinary_build_miss(
    repository: tuple[Path, Path, str], monkeypatch: pytest.MonkeyPatch,
) -> None:
    repo, root, _ = repository
    monkeypatch.setattr(cache.shutil, "which", lambda _: None)
    with cache.automatic_cache(repo) as binding:
        assert binding is None
    with pytest.raises(cache.CacheError, match="install ccache"):
        cache.prepare_cache(repo, successful_build)
    assert not root.exists()


def test_reject_feature_producer_before_storage_creation(repository: tuple[Path, Path, str]) -> None:
    repo, root, _ = repository
    with pytest.raises(cache.CacheError, match="Only origin/main"):
        cache.prepare_cache(repo, successful_build, main_ref="feature")
    assert not root.exists()


def test_consumer_body_error_propagates_without_double_yield(repository: tuple[Path, Path, str]) -> None:
    repo, _, _ = repository
    assert cache.prepare_cache(repo, successful_build) == 0
    with pytest.raises(OSError, match="build failure"):
        with cache.automatic_cache(repo):
            raise OSError("build failure")


def test_unknown_generation_is_preserved(repository: tuple[Path, Path, str]) -> None:
    repo, root, _ = repository
    assert cache.prepare_cache(repo, successful_build) == 0
    unknown = root / "generations" / ("gen-" + "b" * 32)
    unknown.mkdir()
    (unknown / "user-work").write_text("preserve")
    assert cache.prepare_cache(repo, successful_build) == 0
    assert (unknown / "user-work").read_text() == "preserve"


def test_symlinked_root_cannot_publish_or_retire(repository: tuple[Path, Path, str], tmp_path: Path) -> None:
    repo, root, _ = repository
    outside = tmp_path / "outside"
    outside.mkdir()
    try:
        root.symlink_to(outside, target_is_directory=True)
    except OSError:
        pytest.skip("Creating directory symlinks requires host permission")
    with cache.automatic_cache(repo) as binding:
        assert binding is None
    with pytest.raises(cache.CacheError, match="symlinks"):
        cache.prepare_cache(repo, successful_build)
    assert not list(outside.iterdir())


def test_corrupted_configuration_is_safe_miss(repository: tuple[Path, Path, str]) -> None:
    repo, root, _ = repository
    assert cache.prepare_cache(repo, successful_build) == 0
    generation = root / "generations" / cache.status(repo)["generation"]
    (generation / "readonly.conf").write_text("read_only = false\nsloppiness = time_macros\n")
    with cache.automatic_cache(repo) as binding:
        assert binding is None
    assert generation.is_dir()


def test_heavy_lock_is_exclusive_and_releases_after_exception(repository: tuple[Path, Path, str]) -> None:
    repo, root, _ = repository
    with pytest.raises(RuntimeError, match="cancelled"):
        with cache.heavy_build_lock(repo):
            with pytest.raises(BlockingIOError):
                with cache._FileLock(root / "heavy-build.lock", blocking=False):
                    pytest.fail("Concurrent heavy build acquired lock")
            raise RuntimeError("cancelled")
    with cache._FileLock(root / "heavy-build.lock", blocking=False):
        pass


def test_reader_lock_releases_when_build_raises(repository: tuple[Path, Path, str]) -> None:
    repo, root, _ = repository
    assert cache.prepare_cache(repo, successful_build) == 0
    old = root / "generations" / cache.status(repo)["generation"]
    with pytest.raises(RuntimeError, match="cancelled"):
        with cache.automatic_cache(repo):
            raise RuntimeError("cancelled")
    assert cache.prepare_cache(repo, successful_build) == 0
    assert not old.exists()


def test_repository_identity_is_shared_by_linked_worktrees(
    repository: tuple[Path, Path, str], monkeypatch: pytest.MonkeyPatch,
) -> None:
    repo, _, commit = repository
    peer = repo.parent / "peer checkout"
    git(repo, "worktree", "add", "--detach", str(peer), commit)
    # Load a fresh module to recover the unpatched cache-directory implementation.
    spec = importlib.util.spec_from_file_location("horo_cache_identity", ROOT / "scripts" / "local_build_cache.py")
    assert spec and spec.loader
    independent = importlib.util.module_from_spec(spec)
    monkeypatch.setitem(sys.modules, spec.name, independent)
    spec.loader.exec_module(independent)
    assert independent.cache_directory(repo) == independent.cache_directory(peer)
    assert not independent.cache_directory(repo).is_relative_to(repo)


def test_cache_inside_peer_source_is_rejected(
    repository: tuple[Path, Path, str], monkeypatch: pytest.MonkeyPatch,
) -> None:
    repo, _, commit = repository
    peer = repo.parent / "peer checkout"
    git(repo, "worktree", "add", "--detach", str(peer), commit)
    root = peer / "user-cache"
    monkeypatch.setattr(cache, "cache_directory", lambda _: root)
    with pytest.raises(cache.CacheError, match="outside source"):
        cache.prepare_cache(repo, successful_build)
    assert not root.exists()


@pytest.mark.parametrize("readonly", [False, True])
def test_ccache_parses_generated_configuration(tmp_path: Path, readonly: bool) -> None:
    launcher = shutil.which("ccache")
    if launcher is None:
        pytest.skip("ccache is not installed; native configuration parser unavailable")
    config = tmp_path / "ccache.conf"
    config.write_text(cache._config("4G", readonly=readonly), encoding="utf-8")
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith("CCACHE_")}
    env.update({"CCACHE_CONFIGPATH": str(config), "CCACHE_DIR": str(tmp_path / "compiler")})
    for setting, expected in (("read_only", "true" if readonly else "false"),
                              ("compiler_check", "content"), ("sloppiness", ""), ("remote_storage", "")):
        result = subprocess.run([launcher, "--get-config", setting], env=env,
                                capture_output=True, text=True, check=False)
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == expected


def test_cleanup_latch_created_after_gc_guard_preserves_generation(
    repository: tuple[Path, Path, str], monkeypatch: pytest.MonkeyPatch,
) -> None:
    repo, root, _ = repository
    assert cache.prepare_cache(repo, successful_build) == 0
    old = root / "generations" / cache.status(repo)["generation"]
    original_lock = cache._FileLock
    injected = False

    @contextmanager
    def inject_on_exclusive_generation_entry(path: Path, *, shared: bool = False, blocking: bool = True):
        nonlocal injected
        with original_lock(path, shared=shared, blocking=blocking) as lease:
            if path == root / "leases" / f"{old.name}.lock" and not shared:
                # A failed consumer records its latch before releasing its reader lease.
                # GC must inspect that new latch after its exclusive lease is acquired.
                cache.record_cleanup_failure(repo)
                injected = True
            yield lease

    monkeypatch.setattr(cache, "_FileLock", inject_on_exclusive_generation_entry)
    assert cache.prepare_cache(repo, successful_build) == 0
    assert injected
    assert cache.status(repo)["generation"] != old.name
    assert cache.status(repo)["cleanup_blocked"] is True
    assert (old / "compiler" / "entry").read_text() == "object"
