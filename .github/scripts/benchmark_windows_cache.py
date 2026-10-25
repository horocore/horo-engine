"""One-off same-object Windows experiment; this diagnostic branch is never merged."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import time


ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/ci-audio-release"
RESULTS = ROOT / "build/windows-cache-benchmark"
GROUPS = ("src/foundation/", "src/runtime/ui/", "src/physics/", "src/runtime/navigation/")


def run(arguments, output, environment):
    """Execute only fixed toolchain commands; generated target names stay argv data."""
    assert arguments[0] in {"cmake", "sccache", "ccache", "dumpbin"}
    executable = shutil.which(arguments[0])
    if executable is None:
        raise RuntimeError("Required benchmark tool is missing")
    with output.open("w", encoding="utf-8") as log:
        subprocess.run(arguments, executable=executable, cwd=ROOT, env=environment, stdout=log,
                       stderr=subprocess.STDOUT, check=True, timeout=240, shell=False)


def configure_launcher(index, launcher, environment):
    """Change only the launcher and prove every real compiler input is retained."""
    assert launcher in {"sccache", "ccache"}
    hook = RESULTS / "launcher.cmake"
    hook.write_text(
        "function(horo_benchmark_launcher)\n"
        "  set_property(TARGET HoroFoundation HoroRuntimeUi HoroPhysics HoroNavigationApi\n"
        "    PROPERTY CXX_COMPILER_LAUNCHER ${HORO_BENCHMARK_LAUNCHER})\n"
        "endfunction()\n"
        "cmake_language(DEFER CALL horo_benchmark_launcher)\n", encoding="utf-8")
    path = BUILD / "compile_commands.json"
    before = json.loads(path.read_text(encoding="utf-8"))
    run(["cmake", "--preset", "ci-audio-release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
         "-DCMAKE_C_COMPILER_LAUNCHER=sccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=sccache",
         f"-DCMAKE_PROJECT_HoroEngine_INCLUDE={hook}", f"-DHORO_BENCHMARK_LAUNCHER={launcher}"],
        RESULTS / f"{index}-{launcher}.configure.log", environment)
    after = json.loads(path.read_text(encoding="utf-8"))
    if before != after:
        difference = {"before": before, "after": after}
        (RESULTS / "command-differences.json").write_text(json.dumps(difference), encoding="utf-8")
    # CMake compile databases describe the actual compiler, omitting launchers.
    assert before == after, "Changing a launcher changed compiler commands"


def select_objects():
    """Select equal small source groups from actual generated compile commands."""
    commands = json.loads((BUILD / "compile_commands.json").read_text(encoding="utf-8"))
    selected = {}
    for group in GROUPS:
        count = 0
        for command in sorted(commands, key=lambda entry: (entry["file"], entry["output"])):
            source = Path(command["file"]).resolve()
            if not source.is_relative_to(ROOT) or not source.relative_to(ROOT).as_posix().startswith(group):
                continue
            if not Path(command["directory"]).resolve().is_relative_to(BUILD):
                continue
            output = (BUILD / command["output"]).resolve()
            assert output.is_relative_to(BUILD)
            assert output.suffix in {".obj", ".o"}
            target = output.relative_to(BUILD).as_posix()
            selected.setdefault(target, source.relative_to(ROOT).as_posix())
            count += 1
            if count == 12:
                break
        assert count == 12, group
    assert len(selected) == 48
    return selected


def digests(targets):
    """Prove the resulting real object bytes remain identical between modes."""
    result = {}
    for target in targets:
        with (BUILD / target).open("rb") as source:
            result[target] = hashlib.file_digest(source, "sha256").hexdigest()
    return result


def preserve_object_evidence(mode, targets, environment):
    """Keep four representative objects and native COFF diagnostics for inspection."""
    for index, group in enumerate(GROUPS):
        target = next(target for target, source in targets.items() if source.startswith(group))
        snapshot = RESULTS / f"{mode}-group-{index}.obj"
        shutil.copyfile(BUILD / target, snapshot)
        run(["dumpbin", "/all", str(snapshot)], snapshot.with_suffix(".dumpbin.log"), environment)


def invalidation_guard(environment):
    """Require direct-cache invalidation after same-length header changes with preserved mtime."""
    fixture = RESULTS / "invalidation"
    fixture.mkdir(exist_ok=True)
    header = fixture / "value.h"
    source = fixture / "main.cpp"
    source.write_text('#include "value.h"\nint main() { return fixture_value; }\n', encoding="utf-8")
    object_file = fixture / "main.obj"
    executable = fixture / "guard.exe"
    compiler = shutil.which("cl")
    assert compiler is not None
    rows = []
    header.write_text("inline constexpr int fixture_value = 17;\n", encoding="utf-8")
    # Let the initial filesystem timestamp tick pass so the warm lookup can
    # actually use direct mode with its default fresh-include race protection.
    time.sleep(1.1)
    run(["ccache", "--zero-stats"], fixture / "reset.log", environment)
    for index, value in enumerate((17, 17, 29, 17)):
        original = header.stat()
        contents = f"inline constexpr int fixture_value = {value};\n"
        if header.read_text(encoding="utf-8") != contents:
            header.write_text(contents, encoding="utf-8")
        if index >= 2:
            os.utime(header, ns=(original.st_atime_ns, original.st_mtime_ns))
        object_file.unlink(missing_ok=True)
        with (fixture / f"{index}.compile.log").open("w", encoding="utf-8") as log:
            subprocess.run(["ccache", compiler, "/nologo", "/std:c++20", "/EHsc", "/O2", "/Z7", "/c",
                            str(source), f"/Fo{object_file}"], cwd=fixture, env=environment, stdout=log,
                           stderr=subprocess.STDOUT, timeout=60, check=True, shell=False)
        with (fixture / f"{index}.link.log").open("w", encoding="utf-8") as log:
            subprocess.run([compiler, "/nologo", str(object_file), f"/Fe{executable}"], cwd=fixture,
                           env=environment, stdout=log, stderr=subprocess.STDOUT, timeout=60,
                           check=True, shell=False)
        actual = subprocess.run([str(executable)], cwd=fixture, env=environment, timeout=30,
                                check=False, shell=False).returncode
        assert actual == value, "Cache reused stale header contents"
        statistics_path = fixture / f"{index}.stats.log"
        run(["ccache", "--print-stats"], statistics_path, environment)
        if index == 1:
            counters = dict(line.split() for line in statistics_path.read_text(encoding="utf-8").splitlines())
            assert int(counters["direct_cache_hit"]) >= 1, "Warm direct-mode guard was not exercised"
        rows.append({"expected": value, "actual": actual, "mtimePreserved": index >= 2})
    return rows


def measure(index, mode, workers, targets, environment):
    """Rebuild only selected objects; leave compiler storage and flags unchanged."""
    selected = dict(environment)
    selected.pop("SCCACHE_ERROR_LOG", None)
    selected.pop("SCCACHE_LOG", None)
    selected["SCCACHE_CLIENT_SIDE"] = "0"
    launcher = "ccache" if mode == "ccache" else "sccache"
    configure_launcher(index, launcher, selected)
    for target in targets:
        output = BUILD / target
        assert not output.is_symlink()
        output.unlink(missing_ok=True)
    prefix = RESULTS / f"{index}-{mode}-{workers}"
    run([launcher, "--zero-stats"], prefix.with_suffix(".reset.log"), selected)
    started = time.monotonic()
    run(["cmake", "--build", str(BUILD), "--parallel", str(workers), "--target", *targets],
        prefix.with_suffix(".build.log"), selected)
    elapsed = time.monotonic() - started
    run([launcher, "--show-stats"], prefix.with_suffix(".stats.log"), selected)
    return {"mode": mode, "workers": workers, "seconds": elapsed}, digests(targets)


def main():
    RESULTS.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    environment["CCACHE_DIR"] = str(ROOT / "build/windows-ccache-benchmark-store")
    environment["CCACHE_MAXSIZE"] = "256M"
    environment["CCACHE_COMPILERCHECK"] = "content"
    environment["CCACHE_SLOPPINESS"] = ""
    order = [("server", 4), ("ccache", 4)]
    summary = {"scope": "48 real Release objects on one runner, unchanged MSVC commands; no full-job or runtime claim",
               "samples": []}
    try:
        targets = select_objects()
        summary["targets"] = targets
        _, reference = measure("warm", "server", 4, targets, environment)
        preserve_object_evidence("server", targets, environment)
        _, candidate = measure("warm", "ccache", 4, targets, environment)
        preserve_object_evidence("ccache", targets, environment)
        summary["headerInvalidation"] = invalidation_guard(environment)
        summary["coldCacheObjectHashesIdentical"] = candidate == reference
        summary["coldCacheObjectDifferences"] = [target for target in targets if candidate[target] != reference[target]]
        for index, (mode, workers) in enumerate(order + list(reversed(order)) + order + list(reversed(order))):
            sample, actual = measure(index, mode, workers, targets, environment)
            assert actual == (candidate if mode == "ccache" else reference)
            summary["samples"].append(sample)
            print(json.dumps(sample), flush=True)
        summary["mediansSeconds"] = {
            f"{mode}-{workers}": statistics.median(sample["seconds"] for sample in summary["samples"]
                                                   if sample["mode"] == mode and sample["workers"] == workers)
            for mode, workers in order}
        summary["objectsIdenticalWithinEachCache"] = True
        summary["objectSha256"] = reference
        summary["ccacheObjectSha256"] = candidate
    finally:
        (RESULTS / "result.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary), flush=True)


if __name__ == "__main__":
    main()
