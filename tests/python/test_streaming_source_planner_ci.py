"""CI admission for actual streaming planner runtime and benchmark qualification."""
from pathlib import Path
import json

ROOT = Path(__file__).resolve().parents[2]


def test_streaming_source_planner_qualification_is_executed_in_ci() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    suites = (ROOT / "tests/cmake/HoroCiSuites.cmake").read_text(encoding="utf-8")
    windows_group = suites.split("set(HORO_CI_WINDOWS_TARGETS", 1)[1].split(")", 1)[0].split()
    assert {"HoroWorldStreamingTests", "HoroWorldStreamingPublicHeaderConsumer", "HoroStreamingSourcePlannerBenchmark"} <= set(windows_group)
    assert "add_executable(HoroStreamingSourcePlannerBenchmark EXCLUDE_FROM_ALL" not in tests_cmake
    assert "add_test(NAME HoroStreamingSourcePlannerQualification COMMAND HoroStreamingSourcePlannerBenchmark 256)" in tests_cmake
    assert 'LABELS "unit;world_streaming;headless;qualification;ci-windows" TIMEOUT 90' in tests_cmake
    assert 'horo_register_catch_test(HoroWorldStreamingTests LABELS "unit;world_streaming;headless")' in tests_cmake
    presets = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
    for name in ("ci-linux-debug", "ci-macos-debug"):
        preset = next(item for item in presets["testPresets"] if item["name"] == name)
        assert preset["filter"]["exclude"]["label"] == "gui|tooling"
