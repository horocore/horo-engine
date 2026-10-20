"""Actual compiler proof of main checkpoint reuse and source invalidation."""

from __future__ import annotations

import dev

import os
from pathlib import Path
import shutil
# Disposable native fixture runs explicit, validated argv without a shell.
import subprocess  # nosec B404
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import dev as dev
import dev as dev_process
import dev as cache


def run(*arguments: str, cwd: Path, env: dict[str, str] | None = None) -> str:
    command = dev_process.validated_command(arguments, cwd, env)
    # Fixture callers supply Git/CMake argv or the executable built inside their disposable source tree.
    # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit
    process = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True, check=False, shell=False)  # nosec B603
    assert process.returncode == 0, process.stdout + process.stderr
    return process.stdout


def test_real_worktree_cache_hit_and_changed_header_miss(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    if sys.platform == "win32":
        pytest.skip("GCC/Clang cross-directory compiler proof requires a POSIX host")
    compiler = shutil.which("clang++") or shutil.which("g++")
    if not compiler or any(not shutil.which(tool) for tool in ("git", "cmake", "ninja", "ccache")):
        pytest.skip("actual compiler cache proof requires git, CMake, Ninja, ccache and GCC/Clang")
    repo = tmp_path / "main source türkçe"
    repo.mkdir()
    run("git", "init", "-b", "main", cwd=repo)
    run("git", "config", "user.name", "Cache Fixture", cwd=repo)
    run("git", "config", "user.email", "cache@example.invalid", cwd=repo)
    (repo / ".gitignore").write_text("/build/\n")
    (repo / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.25)\nproject(CacheProof LANGUAGES CXX)\nadd_executable(proof main.cpp)\n"
    )
    (repo / "main.cpp").write_text('#include "answer.h"\n#include <cstdio>\nint main() { std::printf("%d", answer); }\n')
    (repo / "answer.h").write_text("inline constexpr int answer = 42;\n")
    run("git", "add", ".", cwd=repo)
    run("git", "commit", "-m", "fixture", cwd=repo)
    commit = run("git", "rev-parse", "HEAD", cwd=repo).strip()
    run("git", "update-ref", "refs/remotes/origin/main", commit, cwd=repo)
    store = tmp_path / "compiler store türkçe"
    monkeypatch.setattr(cache, 'cache_directory', lambda _: store)

    def compile_source(source: Path, build: Path, binding: cache.CacheBinding, stats: Path | None = None) -> None:
        command = ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
                   "-DCMAKE_BUILD_TYPE=Debug", f"-DCMAKE_CXX_COMPILER={compiler}",
                   f"-DCMAKE_CXX_COMPILER_LAUNCHER={binding.launcher}"]
        command = dev._cache_configure_arguments(command, build, source)
        environment = dict(binding.env)
        if stats:
            environment["CCACHE_STATSLOG"] = str(stats)
        run(*command, cwd=source, env=environment)
        run("cmake", "--build", str(build), "--parallel", "1", cwd=source, env=environment)

    def prepare(source: Path, build: Path, binding: cache.CacheBinding) -> int:
        compile_source(source, build, binding)
        assert run(str(build / "proof"), cwd=source).strip() == "42"
        return 0

    assert cache.prepare_cache(repo, prepare) == 0
    feature = tmp_path / "feature source türkçe"
    run("git", "worktree", "add", "--detach", str(feature), commit, cwd=repo)
    try:
        assert cache.status(feature)["main_commit"] == commit
        build = feature / "build" / "dev-editor"
        stats = tmp_path / "consumer-stats.log"
        with cache.automatic_cache(feature) as binding:
            assert binding is not None
            compile_source(feature, build, binding, stats)
        assert "cache_hit" in stats.read_text()
        assert run(str(build / "proof"), cwd=feature).strip() == "42"
        (feature / "answer.h").write_text("inline constexpr int answer = 99;\n")
        # Ensure the header is newer than the built object on coarse timestamp filesystems.
        stamp = (build / "CMakeFiles" / "proof.dir" / "main.cpp.o").stat().st_mtime + 2
        os.utime(feature / "answer.h", (stamp, stamp))
        with cache.automatic_cache(feature) as binding:
            assert binding is not None
            compile_source(feature, build, binding)
        assert run(str(build / "proof"), cwd=feature).strip() == "99"
        assert cache.status(feature)["main_commit"] == commit
    finally:
        # Only this disposable fixture worktree is removed; real repositories are untouched.
        run("git", "worktree", "remove", "--force", str(feature), cwd=repo)
