from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest


SPEC = importlib.util.spec_from_file_location(
    "ci_cache_identity", Path(__file__).resolve().parents[2] / "scripts" / 'dev.py'
)
assert SPEC is not None
assert SPEC.loader is not None
cache = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = cache
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
    path.write_bytes(path.read_bytes().replace(b"\n", bytes((13, 10))))
    assert cache.dependency_fingerprint(source_root) == initial


@pytest.mark.parametrize("compiler,banner,expected", [
    ("g++", "13.3.0\n", "gcc-13.3.0"),
    ("gcc", "13.3.0\n", "gcc-13.3.0"),
    ("clang", "clang version 19.1.7", "clang-19.1.7"),
    ("clang++", "Apple clang version 17.0.0 (clang-1700)", "appleclang-17.0.0"),
    ("cl", "Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35207 for x64", "msvc-19.44.35207"),
])
def test_compiler_identity_uses_active_full_version(
    compiler: str, banner: str, expected: str
) -> None:
    assert cache.compiler_identity(compiler, banner) == expected


@pytest.mark.parametrize("compiler", ["gcc", "g++", "clang", "clang++", "cl"])
def test_unrecognized_version_cannot_restore_an_unidentified_namespace(compiler: str) -> None:
    with pytest.raises(ValueError, match="Cannot identify"):
        cache.compiler_identity(compiler, "broken")


@pytest.mark.parametrize("compiler", ["gcc; echo injected", "toolchain/g++", "cl --help", "unknown"])
def test_only_matrix_compiler_names_are_accepted(compiler: str) -> None:
    with pytest.raises(ValueError, match="Unsupported CI compiler"):
        cache.compiler_identity(compiler, "13.3.0")


def identity_arguments(output: Path, cmake_version: str = "cmake version 3.31.6") -> list[str]:
    return [
        "--github-output", str(output), "--cc", "gcc", "--cxx", "g++",
        "--cc-version", "13.3.0", "--cxx-version", "13.3.0", "--cmake-version", cmake_version,
    ]


def test_main_appends_validated_outputs_with_spaces_and_unicode(tmp_path: Path) -> None:
    output = tmp_path / "cache çıktısı.txt"
    output.write_text("existing=value\n", encoding="utf-8")
    cache.ci_cache_main(identity_arguments(output))
    assert output.read_text(encoding="utf-8").splitlines() == [
        "existing=value", "compiler=gcc-13.3.0", "c-compiler=gcc-13.3.0", "cmake=3.31",
        f"dependencies={cache.dependency_fingerprint(Path(__file__).resolve().parents[2])}",
    ]


def test_invalid_cmake_version_does_not_write_outputs(tmp_path: Path) -> None:
    output = tmp_path / "outputs.txt"
    arguments = identity_arguments(output, "broken")
    with pytest.raises(ValueError, match="Cannot identify CMake"):
        cache.ci_cache_main(arguments)
    assert not output.exists()


def test_invalid_compiler_version_does_not_write_partial_outputs(tmp_path: Path) -> None:
    output = tmp_path / "outputs.txt"
    arguments = identity_arguments(output)
    arguments[arguments.index("--cxx-version") + 1] = "broken"
    with pytest.raises(ValueError, match="Cannot identify compiler"):
        cache.ci_cache_main(arguments)
    assert not output.exists()


def test_missing_output_parent_reports_failure(tmp_path: Path) -> None:
    arguments = identity_arguments(tmp_path / "missing" / "outputs.txt")
    with pytest.raises(FileNotFoundError):
        cache.ci_cache_main(arguments)


def test_source_editor_pin_participates_in_dependency_identity(source_root: Path) -> None:
    helper = source_root / "cmake/SourceEditorWidgetDependency.cmake"
    helper.write_text('set(HORO_ZEP_REVISION "first")\n')
    initial = cache.dependency_fingerprint(source_root)
    helper.write_text('set(HORO_ZEP_REVISION "second")\n')
    assert cache.dependency_fingerprint(source_root) != initial


def test_sonar_dependency_restore_requires_exact_inputs() -> None:
    root = Path(__file__).resolve().parents[2]
    workflow = (root / ".github/workflows/sonar.yml").read_text()
    restore = workflow.split("- name: Restore FetchContent dependencies", 1)[1].split("- name: Identify instrumented compilers", 1)[0]
    assert "restore-keys:" not in restore
    for pattern in ("CMakeLists.txt", "cmake/Dependencies.cmake", "cmake/*Dependency.cmake",
                    "cmake/Horo*DependencyPolicy.cmake", "cmake/HoroGnsProtobufConfig.cmake"):
        assert f"'{pattern}'" in restore


def test_sonar_consumer_and_direct_publication_share_actual_compiler_identity() -> None:
    root = Path(__file__).resolve().parents[2]
    sonar = (root / ".github/workflows/sonar.yml").read_text(encoding="utf-8")
    namespace = ("sonar-v3-${{ runner.os }}-${{ runner.arch }}-"
                 "${{ steps.compiler_identity.outputs.c-compiler }}-"
                 "${{ steps.compiler_identity.outputs.compiler }}-main-")
    assert "id: compiler_identity" in sonar
    assert '--cc gcc --cxx g++ --cc-version "$(gcc -dumpfullversion -dumpversion)"' in sonar
    assert '--cxx-version "$(g++ -dumpfullversion -dumpversion)"' in sonar
    assert f"key: {namespace}${{{{ github.run_id }}}}-${{{{ github.run_attempt }}}}" in sonar
    assert f"COMPILER_NAMESPACE: {namespace}" in sonar
    assert sonar.index("name: Identify instrumented compilers") < sonar.index("name: Restore optional public compiler checkpoint")
    assert "runs-on: &sonar_runner ubuntu-26.04" in sonar
    assert "      CC: gcc\n      CXX: g++" in sonar
    assert "Restore compiler cache" not in sonar
    assert "Save compiler cache" not in sonar
