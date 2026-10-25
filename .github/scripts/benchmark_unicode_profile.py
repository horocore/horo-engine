"""Measure private ICU instrumentation without changing first-party coverage inputs."""

import json
from pathlib import Path
import shlex
import shutil
import statistics
import subprocess
import time

from defusedxml import ElementTree


ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/sonar"
RESULTS = ROOT / "build/unicode-profile-benchmark"
TARGETS = ("HoroRuntimeUiTextUnicodeTests", "HoroRuntimeUiUnicodeStartupTests",
           "HoroRuntimeUiUnicodeLifecycleTests")
PATTERN = "^HoroRuntimeUi(TextUnicode|UnicodeStartup|UnicodeLifecycle)Tests::"
EXCLUSION = "-fprofile-exclude-files=.*"


def run(arguments, output):
    """Keep subprocess selection fixed; preserve diagnostics outside the source tree."""
    assert arguments[0] in {"cmake", "ctest", "bash"}
    executable = shutil.which(arguments[0])
    if executable is None:
        raise RuntimeError("Required experiment tool is missing")
    with output.open("w", encoding="utf-8") as log:
        subprocess.run(arguments, executable=executable, cwd=ROOT, stdout=log,
                       stderr=subprocess.STDOUT, timeout=600, check=True, shell=False)


def configure(index, mode):
    """Use the same preset, paths, revision and flags, changing only private ICU arcs."""
    hook = ROOT / ".github/scripts/benchmark_unicode_profile.cmake"
    run(["cmake", "--preset", "sonar", "-DPython3_EXECUTABLE=/tmp/horo-coverage/bin/python",
         f"-DCMAKE_PROJECT_HoroEngine_INCLUDE={hook}",
         f"-DHORO_BENCHMARK_UNICODE_EXCLUSION={'ON' if mode == 'excluded' else 'OFF'}"],
        RESULTS / f"{index}-{mode}.configure.log")
    commands = json.loads((BUILD / "compile_commands.json").read_text(encoding="utf-8"))
    return {(entry["file"], entry["output"]): shlex.split(entry["command"])
            for entry in commands}


def verify_commands(reference, actual, mode):
    """Reject changes to any compiler argument other than the vendor-only option."""
    assert reference.keys() == actual.keys()
    changed = []
    for key, command in actual.items():
        if command == reference[key]:
            continue
        assert mode == "excluded" and "CMakeFiles/HoroThirdPartyUnicode.dir/" in key[1]
        assert command.count(EXCLUSION) == 1
        assert [word for word in command if word != EXCLUSION] == reference[key]
        changed.append(key[0])
    assert len(changed) == (203 if mode == "excluded" else 0)
    return len(changed)


def clear_counters():
    """Reset only owned build gcda counters; never follow an escaping path or symlink."""
    assert BUILD.resolve().is_relative_to(ROOT.resolve()) and not BUILD.is_symlink()
    for path in BUILD.rglob("*.gcda"):
        assert path.resolve().is_relative_to(BUILD.resolve()) and not path.is_symlink()
        path.unlink()


def coverage_lines(path):
    """Read the same validated generic coverage format submitted by production CI."""
    assert path.stat().st_size < 100 * 1024 * 1024
    document = ElementTree.fromstring(path.read_bytes())
    assert document.tag == "coverage" and document.get("version") == "1"
    result = {}
    for source in document.findall("file"):
        name = source.attrib["path"]
        assert (ROOT / name).resolve().is_relative_to(ROOT.resolve())
        for line in source.findall("lineToCover"):
            assert line.attrib["covered"] in {"true", "false"}
            result[(name, int(line.attrib["lineNumber"]))] = line.attrib["covered"] == "true"
    assert result
    return result


def test_outcomes(path):
    """Require exactly the same passing, non-skipped named workload on each pass."""
    document = ElementTree.fromstring(path.read_bytes())
    cases = document.findall(".//testcase")
    assert len(cases) == 16
    assert all(case.find("failure") is None and case.find("error") is None
               and case.find("skipped") is None for case in cases)
    return sorted((case.get("classname"), case.attrib["name"]) for case in cases)


def sample(index, mode, reference):
    """Measure real Unicode behavior and collect owned coverage after the same tests."""
    commands = configure(index, mode)
    changed = verify_commands(reference or commands, commands, mode)
    prefix = RESULTS / f"{index}-{mode}"
    run(["cmake", "--build", str(BUILD), "--parallel", "4", "--target", *TARGETS],
        prefix.with_suffix(".build.log"))
    clear_counters()
    junit = prefix.with_suffix(".junit.xml")
    started = time.monotonic()
    run(["ctest", "--test-dir", str(BUILD), "--parallel", "4", "--output-on-failure",
         "--no-tests=error", "--output-junit", str(junit), "-R", PATTERN],
        prefix.with_suffix(".tests.log"))
    test_seconds = time.monotonic() - started
    outcomes = test_outcomes(junit)
    vendor = list((BUILD / "CMakeFiles/HoroThirdPartyUnicode.dir").rglob("*.gcda"))
    started = time.monotonic()
    run(["bash", ".github/scripts/collect_sonar_coverage.sh"], prefix.with_suffix(".coverage.log"))
    coverage_seconds = time.monotonic() - started
    report = prefix.with_suffix(".coverage.xml")
    shutil.copyfile(BUILD / "coverage.xml", report)
    lines = coverage_lines(report)
    result = {"mode": mode, "testsSeconds": test_seconds, "coverageSeconds": coverage_seconds,
              "passingTests": len(outcomes), "measuredLines": len(lines), "coveredLines": sum(lines.values()),
              "changedVendorCompileCommands": changed, "vendorCounterFiles": len(vendor),
              "vendorCounterBytes": sum(path.stat().st_size for path in vendor)}
    return result, commands, outcomes, lines


def compare_coverage(reference, actual, index):
    """Preserve all coverage differences rather than interpreting a percentage as proof."""
    common = reference.keys() & actual.keys()
    return {"sample": index, "missing": sorted(reference.keys() - actual.keys()),
            "extra": sorted(actual.keys() - reference.keys()),
            "lostCovered": sorted(key for key in common if reference[key] and not actual[key]),
            "gainedCovered": sorted(key for key in common if not reference[key] and actual[key])}


def main():
    """Run a balanced baseline/candidate/candidate/baseline experiment on one runner."""
    RESULTS.mkdir(parents=True, exist_ok=True)
    summary = {"scope": "One-runner 16-case Unicode workload; not full-suite qualification", "samples": [],
               "coverageDifferences": []}
    reference = outcomes = lines = None
    try:
        for index, mode in enumerate(("baseline", "excluded", "excluded", "baseline")):
            measured, commands, actual_outcomes, actual_lines = sample(index, mode, reference)
            if reference is None:
                reference, outcomes, lines = commands, actual_outcomes, actual_lines
            assert actual_outcomes == outcomes
            difference = compare_coverage(lines, actual_lines, index)
            summary["coverageDifferences"].append(difference)
            assert not difference["missing"] and not difference["extra"], "Owned measured-line scope changed"
            summary["samples"].append(measured)
            print(json.dumps(measured), flush=True)
        summary["medians"] = {mode: {metric: statistics.median(row[metric] for row in summary["samples"]
                                                               if row["mode"] == mode)
                                      for metric in ("testsSeconds", "coverageSeconds")}
                              for mode in ("baseline", "excluded")}
    finally:
        (RESULTS / "result.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary), flush=True)


if __name__ == "__main__":
    main()
