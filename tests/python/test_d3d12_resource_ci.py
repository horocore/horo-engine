"""Keep native D3D12 resource qualification independent of the broad platform suite."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]


def test_d3d12_resource_job_builds_and_executes_real_warp_tests() -> None:
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    match = re.search(r"^  directx-resources:\n(.*?)(?=^  [a-z-]+:\n|\Z)", workflow, re.M | re.S)
    assert match, "Missing independent native D3D12 resource qualification"
    job = match.group(1)
    assert "runs-on: windows-2022" in job
    assert "timeout-minutes: 45" in job
    assert "cmake --preset ci-windows-debug -B build/ci-d3d12-resources" in job
    assert "cmake --build build/ci-d3d12-resources --parallel 2" in job
    assert "HoroD3D12InitializationTests HoroD3D12ResourceTests" in job
    assert "ctest --test-dir build/ci-d3d12-resources" in job
    assert "--no-tests=error" in job
    assert "-R '^HoroD3D12InitializationTests::'" in job
    assert "tests/HoroD3D12ResourceTests.exe '[warp]'" in job
    assert "--reporter junit --out build/ci-d3d12-resources/warp.xml" in job
    assert "d3d12-resource-results" in job
    assert "build/ci-d3d12-resources/*.xml" in job
    assert "build/ci-d3d12-resources/Testing/Temporary/LastTest.log" in job
    assert "continue-on-error" not in job
    assert "--allow-running-no-tests" not in job
