"""Execute CTest against the real suite-property generator without engine builds."""
from __future__ import annotations

import json
import re
from pathlib import Path
import shutil
import subprocess  # nosec B404 - fixed tool probes and locally compiled test fixtures.
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.skipif(not shutil.which("cmake") or not shutil.which("ctest"), reason="CMake/CTest required")

PROBE = '''import json, os, pathlib, sys, time
group, name, output = sys.argv[1:]
output = pathlib.Path(output)
work = pathlib.Path.cwd()
temporary = pathlib.Path(os.environ["TMPDIR"])
assert temporary == pathlib.Path(os.environ["TMP"]) == pathlib.Path(os.environ["TEMP"])
assert pathlib.Path(os.environ["HOME"]) == pathlib.Path(os.environ["USERPROFILE"])
assert os.environ["PRESERVED_SETTING"] == "present"
assert os.environ["EXISTING"] == "retained"
assert work.name == "work" and work.parent.name == group
assert temporary.parent == work.parent
marker = work / "shared-fixture"
assert not marker.exists(), "cases within one executable overlapped"
marker.write_text(name)
started = time.monotonic()
(output / (group + ".ready")).touch()
deadline = time.monotonic() + 5
while len(list(output.glob("*.ready"))) < 2:
    assert time.monotonic() < deadline, "independent executables did not overlap"
    time.sleep(0.01)
time.sleep(0.1)
assert marker.read_text() == name
marker.unlink()
(output / (name + ".json")).write_text(json.dumps({"start": started, "end": time.monotonic(), "group": group}))
'''


def run(tool: str, *args: str, cwd: Path | None = None) -> subprocess.CompletedProcess:
    if tool not in {"cmake", "ctest", "gcc", "gcov", "readelf"}:
        raise ValueError("Only fixed qualification tools may be executed")
    executable = shutil.which(tool)
    if executable is None:
        raise RuntimeError(f"Missing qualification tool: {tool}")
    command = [str(Path(executable).resolve(strict=True)), *args]
    # All arguments originate in this fixture; no shell or external command input.
    # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit
    return subprocess.run(command, cwd=cwd, capture_output=True, text=True, check=True, shell=False, timeout=30)  # nosec B603


def prepare_isolation_project(tmp_path: Path, isolate: bool) -> tuple[Path, Path]:
    source = tmp_path / "source ü"
    source.mkdir()
    build = tmp_path / "build ü"
    evidence = tmp_path / "evidence"
    evidence.mkdir()
    probe = source / "probe.py"
    probe.write_text(PROBE, encoding="utf-8")
    registry_path = source / "registry.cmake"
    lines = [
        "cmake_minimum_required(VERSION 3.25)", "project(IsolationContract NONE)", "enable_testing()",
        f'set(HORO_CI_ISOLATE_TEST_EXECUTABLES {"ON" if isolate else "OFF"} CACHE BOOL "" FORCE)',
        f'include("{(ROOT / "tests/cmake/HoroCiSuites.cmake").as_posix()}")',
        f'set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "{registry_path.as_posix()}")',
    ]
    registry = []
    for group in ("SuiteA", "SuiteB", "Gpu"):
        names = ("one",) if group == "Gpu" else ("one", "two")
        registry.append(f'set({group}_TESTS {" ".join(f"{group}::{name}" for name in names)})')
        for name in names:
            registry.append(f'add_test({group}::{name} "{Path(sys.executable).as_posix()}" "{probe.as_posix()}" {group} {group}-{name} "{evidence.as_posix()}")')
        lines.extend([
            f'set(ARG_LABELS {"gpu" if group == "Gpu" else "native"})',
            f'set(ARG_PROPERTIES TIMEOUT 10 ENVIRONMENT "PRESERVED_SETTING=present" RESOURCE_LOCK {group}-existing ENVIRONMENT_MODIFICATION "EXISTING=set:retained")',
            f'horo_ci_catch_properties({group})',
        ])
    registry_path.write_text("\n".join(registry), encoding="utf-8")
    (source / "CMakeLists.txt").write_text("\n".join(lines), encoding="utf-8")
    run("cmake", "-S", str(source), "-B", str(build))
    return build, evidence


@pytest.mark.parametrize("isolate", [True, False])
def test_generated_suite_properties_preserve_contracts_and_control_concurrency(tmp_path: Path, isolate: bool) -> None:
    build, evidence = prepare_isolation_project(tmp_path, isolate)
    manifest = json.loads(run("ctest", "--test-dir", str(build), "--show-only=json-v1").stdout)
    assert {test["name"] for test in manifest["tests"]} == {"SuiteA::one", "SuiteA::two", "SuiteB::one", "SuiteB::two", "Gpu::one"}
    for test in manifest["tests"]:
        properties = {item["name"]: item["value"] for item in test["properties"]}
        assert properties["TIMEOUT"] == 10
        assert properties["ENVIRONMENT"] == ["PRESERVED_SETTING=present"]
        if isolate:
            group = test["name"].split("::")[0]
            assert properties["RESOURCE_LOCK"] == sorted([f"{group}-existing", f"horo-suite-{group}"])
            assert Path(properties["WORKING_DIRECTORY"]).name == "work"
            assert len(properties["ENVIRONMENT_MODIFICATION"]) == 6
            assert properties.get("RUN_SERIAL", False) == (group == "Gpu")
        else:
            group = test["name"].split("::")[0]
            assert properties["RESOURCE_LOCK"] == [f"{group}-existing"]
            assert properties["ENVIRONMENT_MODIFICATION"] == ["EXISTING=set:retained"]
    if isolate:
        run("ctest", "--test-dir", str(build), "--parallel", "2", "--output-on-failure", "-R", "^Suite")
        events = {path.stem: json.loads(path.read_text()) for path in evidence.glob("*.json")}
        assert len(events) == 4
        for group in ("SuiteA", "SuiteB"):
            intervals = sorted((event["start"], event["end"]) for event in events.values() if event["group"] == group)
            assert intervals[0][1] <= intervals[1][0]
        assert any(max(a["start"], b["start"]) < min(a["end"], b["end"])
                   for a in events.values() for b in events.values() if a["group"] != b["group"])


@pytest.mark.parametrize("linker", ["bfd", "lld"])
def test_gcc_parallel_executables_merge_shared_library_coverage(tmp_path: Path, linker: str) -> None:
    """Protect Sonar counters shared by independently linked test executables."""
    import gzip

    if sys.platform != "linux" or not shutil.which("gcc") or not shutil.which("gcov"):
        pytest.skip("Linux GCC coverage contract")
    if not shutil.which(f"ld.{linker}"):
        pytest.skip(f"GCC linker unavailable: {linker}")
    source = tmp_path / "counter.c"
    source.write_text("int observed(void) { return 1; }\n", encoding="utf-8")
    main = tmp_path / "main.c"
    main.write_text("int observed(void); int main(void) { for (int i = 0; i < 10000; ++i) if (observed() != 1) return 1; return 0; }\n", encoding="utf-8")
    obj = tmp_path / "counter.o"
    run("gcc", "--coverage", "-fprofile-abs-path", "-c", str(source), "-o", str(obj))
    executables = [tmp_path / "suite-a", tmp_path / "suite-b"]
    for executable in executables:
        run("gcc", f"-fuse-ld={linker}", "--coverage", str(main), str(obj), "-o", str(executable))
    for _ in range(4):
        # Only absolute executable paths just compiled from the literal C fixture.
        # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit
        children = [subprocess.Popen([str(executable)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=False)  # nosec B603
                    for executable in executables]
        try:
            for child in children:
                _, stderr = child.communicate(timeout=10)
                assert child.returncode == 0 and not stderr, stderr.decode(errors="replace")
        finally:
            for child in children:
                if child.poll() is None:
                    child.kill()
                    child.wait()
    run("gcov", "--json-format", str(tmp_path / "counter.gcda"), cwd=tmp_path)
    report = json.loads(gzip.decompress((tmp_path / "counter.gcov.json.gz").read_bytes()))
    observed = [function for file in report["files"] for function in file["functions"]
                if function["name"] == "observed"]
    assert len(observed) == 1
    assert observed[0]["execution_count"] == 80000


def test_cmake_lld_links_coverage_executable_shared_and_module_targets(tmp_path: Path) -> None:
    if sys.platform != "linux" or not shutil.which("ld.lld") or not shutil.which("g++"):
        pytest.skip("Linux GCC/LLD qualification")
    version = tuple(int(part) for part in run("cmake", "--version").stdout.split()[2].split(".")[:2])
    if version < (3, 29):
        pytest.skip("CI presets require CMake 3.29 or newer")
    source = tmp_path / "linker source"
    source.mkdir()
    build = tmp_path / "linker build"
    (source / "library.cpp").write_text('extern "C" int answer() { return 42; }\n', encoding="utf-8")
    (source / "main.cpp").write_text(
        '#include <dlfcn.h>\nextern "C" int answer();\n'
        'int main(int argc, char **argv) { if (argc != 2 || answer() != 42) return 1; '
        'void *module = dlopen(argv[1], RTLD_NOW); if (!module) return 2; '
        'auto function = reinterpret_cast<int (*)()>(dlsym(module, "answer")); '
        'int result = function && function() == 42 ? 0 : 3; dlclose(module); return result; }\n', encoding="utf-8")
    (source / "CMakeLists.txt").write_text("""cmake_minimum_required(VERSION 3.29)
project(LinkerContract CXX)
enable_testing()
add_library(archive STATIC library.cpp)
add_library(shared SHARED library.cpp)
add_library(plugin MODULE library.cpp)
add_executable(static_probe main.cpp)
target_link_libraries(static_probe PRIVATE archive ${CMAKE_DL_LIBS})
add_executable(shared_probe main.cpp)
target_link_libraries(shared_probe PRIVATE shared ${CMAKE_DL_LIBS})
add_test(NAME static COMMAND static_probe $<TARGET_FILE:plugin>)
add_test(NAME shared COMMAND shared_probe $<TARGET_FILE:plugin>)
""", encoding="utf-8")
    presets = json.loads((ROOT / "CMakePresets.json").read_text())
    settings = next(item for item in presets["configurePresets"] if item["name"] == "ci-linux-debug")["cacheVariables"]
    linker_flags = [f"-D{key}={value}" for key, value in settings.items() if key.startswith("CMAKE_")]
    run("cmake", "-S", str(source), "-B", str(build), "-DCMAKE_CXX_COMPILER=g++",
        "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_CXX_FLAGS_DEBUG=--coverage -fprofile-abs-path", *linker_flags)
    run("cmake", "--build", str(build), "--parallel", "2")
    run("ctest", "--test-dir", str(build), "--output-on-failure")
    for output in ("static_probe", "shared_probe", "libshared.so", "libplugin.so"):
        assert re.search(r"Linker: (?:Ubuntu )?LLD [0-9]", run("readelf", "--string-dump=.comment", str(build / output)).stdout)
