"""One-off same-binary experiment; this diagnostic branch is never merged."""

import hashlib
import json
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import time


ROOT = Path(__file__).resolve().parents[2]
RESULTS = ROOT / "build/physics-benchmark"
BUILD = ROOT / "build/ci"
REGISTRY = ROOT / "tests/cmake/HoroCiSuites.cmake"
MARKER = "set(HORO_CI_CASE_ISOLATED_TARGETS HoroPhysicsTests)"


def run(arguments, output, timeout=90):
    """Run fixed benchmark tools, preserving complete output outside source."""
    executable = shutil.which(arguments[0])
    if executable is None:
        raise RuntimeError("Required benchmark tool is missing")
    with output.open("w", encoding="utf-8") as log:
        subprocess.run(arguments, executable=executable, cwd=ROOT, stdout=log,
                       stderr=subprocess.STDOUT, check=True, timeout=timeout, shell=False)


def manifest(output):
    run(["ctest", "--test-dir", str(BUILD), "--show-only=json-v1", "-R", "^HoroPhysicsTests::"], output)
    return json.loads(output.read_text(encoding="utf-8"))["tests"]


def outcomes(path):
    """Compare every completed case with discovery, including existing skips."""
    cases = re.findall(r"\d+/\d+ Test\s+#\d+: (.*?)\s+\.{2,}\s*(Passed|\*\*\*Skipped)\s+[\d.]+ sec",
                       path.read_text(encoding="utf-8"))
    assert cases
    result = {name: status != "Passed" for name, status in cases}
    assert len(result) == len(cases)
    return result


def measure(index, mode, original, original_costs):
    """Switch only generated isolation metadata; retain binary and scheduler costs."""
    selected = original if mode == "case" else original.replace(MARKER, "set(HORO_CI_CASE_ISOLATED_TARGETS)")
    REGISTRY.write_text(selected, encoding="utf-8")
    run(["cmake", "--preset", "ci-linux-debug"], RESULTS / f"{index}-configure.log")
    tests = manifest(RESULTS / f"{index}-manifest.json")
    assert tests
    executables = {test["command"][0] for test in tests}
    assert len(executables) == 1
    with Path(next(iter(executables))).open("rb") as binary:
        binary_digest = hashlib.file_digest(binary, "sha256").hexdigest()
    for test in tests:
        properties = {item["name"]: item["value"] for item in test["properties"]}
        suite_lock = "horo-suite-HoroPhysicsTests" in properties.get("RESOURCE_LOCK", [])
        assert suite_lock == (mode == "suite")
    costs = BUILD / "Testing/Temporary/CTestCostData.txt"
    if original_costs is None:
        costs.unlink(missing_ok=True)
    else:
        costs.write_bytes(original_costs)
    report = RESULTS / f"{index}-{mode}.xml"
    started = time.monotonic()
    run(["ctest", "--preset", "ci-linux-debug", "-R", "^HoroPhysicsTests::",
         "--parallel", "4", "--output-junit", str(report)], RESULTS / f"{index}-{mode}.log", timeout=120)
    elapsed = time.monotonic() - started
    actual = outcomes(RESULTS / f"{index}-{mode}.log")
    assert set(actual) == {test["name"] for test in tests}
    return {"mode": mode, "seconds": elapsed, "cases": len(actual), "skipped": sum(actual.values()),
            "binarySha256": binary_digest}, actual


def main():
    RESULTS.mkdir(parents=True, exist_ok=True)
    original = REGISTRY.read_text(encoding="utf-8")
    assert original.count(MARKER) == 1
    costs = BUILD / "Testing/Temporary/CTestCostData.txt"
    original_costs = costs.read_bytes() if costs.exists() else None
    summary = {"workload": "All discovered HoroPhysicsTests cases, Debug, GCC13, four CTest workers",
               "order": ["suite", "case", "case", "suite"], "samples": [],
               "scope": "Same binary and runner; no full-job or instrumented-coverage speedup claim"}
    reference = None
    try:
        for index, mode in enumerate(summary["order"]):
            sample, actual = measure(index, mode, original, original_costs)
            if reference is None:
                reference = actual
            assert actual == reference
            summary["samples"].append(sample)
            print(json.dumps(summary["samples"][-1]), flush=True)
        summary["mediansSeconds"] = {
            mode: statistics.median(sample["seconds"] for sample in summary["samples"] if sample["mode"] == mode)
            for mode in ("suite", "case")}
        assert len({sample["binarySha256"] for sample in summary["samples"]}) == 1
        summary["outcomesIdentical"] = True
    finally:
        REGISTRY.write_text(original, encoding="utf-8")
        (RESULTS / "result.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary), flush=True)


if __name__ == "__main__":
    main()
