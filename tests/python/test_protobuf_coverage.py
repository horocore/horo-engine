"""Registration contracts for executable CMake coverage scope models."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_protobuf_coverage_models_run_in_native_and_tooling_gates():
    registration = (ROOT / "tests/cmake/HoroProtobufCoverageTests.cmake").read_text(encoding="utf-8")
    models = (
        "GNU15Coverage GNU 15.2.0 ON ON",
        "GNU14Coverage GNU 14.3.0 ON OFF",
        "GNU15NoCoverage GNU 15.2.0 OFF OFF",
        "ClangCoverage Clang 20.1.0 ON OFF",
        "AppleClangCoverage AppleClang 17.0.0 ON OFF",
        "MsvcCoverage MSVC 19.44 ON OFF",
    )
    for model in models:
        assert f"horo_add_protobuf_coverage_model({model})" in registration
    assert registration.count("\nhoro_add_protobuf_coverage_model(") == len(models)
    assert 'LABELS "cmake;ci-windows"' in registration
    native = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    assert 'include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/HoroProtobufCoverageTests.cmake")' in native
    standalone = (ROOT / "tests/cmake/protobuf_coverage/CMakeLists.txt").read_text(encoding="utf-8")
    assert 'include("${CMAKE_CURRENT_LIST_DIR}/../HoroProtobufCoverageTests.cmake")' in standalone
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    tooling = workflow.split("  tooling:\n", 1)[1].split("  android-storage:\n", 1)[0]
    assert "cmake -S tests/cmake/protobuf_coverage -B build/protobuf-coverage" in tooling
    assert "ctest --test-dir build/protobuf-coverage --output-on-failure --no-tests=error" in tooling


def test_protobuf_policy_is_applied_after_upstream_target_creation():
    dependencies = (ROOT / "cmake/Dependencies.cmake").read_text(encoding="utf-8")
    creation = 'add_subdirectory("${horo_protobuf_SOURCE_DIR}" "${horo_protobuf_BINARY_DIR}")'
    assert creation + "\n    include(HoroProtobufCoverage)" in dependencies
