from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess

import pytest


SPEC = importlib.util.spec_from_file_location(
    "ci_cache_identity", Path(__file__).resolve().parents[2] / "scripts" / "ci_cache_identity.py"
)
assert SPEC is not None and SPEC.loader is not None
cache = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cache)


@pytest.fixture
def source_root(tmp_path: Path) -> Path:
    (tmp_path / "cmake").mkdir()
    (tmp_path / "CMakeLists.txt").write_text("option(HORO_BUILD_NETWORK_GNS OFF)\n")
    (tmp_path / "cmake/Dependencies.cmake").write_text('set(REVISION "abc")\n')
    (tmp_path / "cmake/HoroGnsProtobufConfig.cmake").write_text("# pinned protobuf setup\n")
    return tmp_path


def test_dependency_changes_invalidate_but_engine_target_changes_do_not(source_root: Path) -> None:
    initial = cache.dependency_fingerprint(source_root)
    (source_root / "src").mkdir()
    (source_root / "src/CMakeLists.txt").write_text("add_library(NewTarget NewTarget.cpp)\n")
    assert cache.dependency_fingerprint(source_root) == initial
    (source_root / "cmake/Dependencies.cmake").write_text('set(REVISION "def")\n')
    assert cache.dependency_fingerprint(source_root) != initial


def test_new_dependency_helper_and_capability_defaults_invalidate(source_root: Path) -> None:
    initial = cache.dependency_fingerprint(source_root)
    helper = source_root / "cmake/HoroPhysicsDependency.cmake"
    helper.write_text('FetchContent_Declare(jolt GIT_TAG "abc")\n')
    with_helper = cache.dependency_fingerprint(source_root)
    assert with_helper != initial
    helper.rename(source_root / "cmake/HoroNavigationDependency.cmake")
    renamed_helper = cache.dependency_fingerprint(source_root)
    assert renamed_helper != with_helper
    (source_root / "CMakeLists.txt").write_text("option(HORO_BUILD_NETWORK_GNS ON)\n")
    assert cache.dependency_fingerprint(source_root) != renamed_helper


def test_dependency_fingerprint_normalizes_windows_line_endings(source_root: Path) -> None:
    initial = cache.dependency_fingerprint(source_root)
    path = source_root / "cmake/Dependencies.cmake"
    path.write_bytes(path.read_bytes().replace(b"\n", b"\r\n"))
    assert cache.dependency_fingerprint(source_root) == initial


@pytest.mark.parametrize("compiler,banner,version,expected", [
    ("g++", "g++ (Ubuntu 13.3.0) 13.3.0", "13.3.0", "gcc-13.3.0"),
    ("clang++", "clang version 19.1.7", None, "clang-19.1.7"),
    ("clang++", "Apple clang version 17.0.0 (clang-1700)", None, "appleclang-17.0.0"),
    ("cl", "Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35207 for x64", None, "msvc-19.44.35207"),
])
def test_compiler_identity_uses_active_full_version(
    monkeypatch: pytest.MonkeyPatch, compiler: str, banner: str, version: str | None, expected: str
) -> None:
    monkeypatch.setattr(cache.subprocess, "run", lambda *args, **kwargs: subprocess.CompletedProcess(
        args[0], 2 if compiler == "cl" else 0, "", banner
    ))
    monkeypatch.setattr(cache.subprocess, "check_output", lambda *args, **kwargs: version)
    assert cache.compiler_identity(compiler) == expected


def test_unrecognized_compiler_cannot_restore_an_unidentified_namespace(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(cache.subprocess, "run", lambda *args, **kwargs: subprocess.CompletedProcess(args[0], 2, "", "broken"))
    with pytest.raises(ValueError, match="Cannot identify"):
        cache.compiler_identity("cl")
