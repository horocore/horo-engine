"""Qualify private case namespaces against the actual CTest property generator."""
from __future__ import annotations

import gzip
import hashlib
import json
from pathlib import Path
import shutil
# Exception type only; the fixed-tool helper owns process creation.
from subprocess import CalledProcessError  # nosec B404
import sys

import pytest

from test_ci_test_isolation import ROOT, run

pytestmark = pytest.mark.skipif(not shutil.which("cmake") or not shutil.which("ctest"), reason="CMake/CTest required")
NAMES = ("case one", "case_one", "case/path [literal] ü", "case;punctuation")
PROBE = '''import hashlib, json, os, pathlib, sys, time
name, output, index, peers = sys.argv[1:]
output = pathlib.Path(output)
work = pathlib.Path.cwd()
temporary = pathlib.Path(os.environ["TMPDIR"])
home = pathlib.Path(os.environ["HOME"])
assert temporary == pathlib.Path(os.environ["TMP"]) == pathlib.Path(os.environ["TEMP"])
assert home == pathlib.Path(os.environ["USERPROFILE"])
assert os.environ["PRESERVED_SETTING"] == "present"
assert os.environ["EXISTING"] == "retained"
assert work.parent.name == hashlib.sha256(name.encode()).hexdigest()
assert temporary.parent == home.parent == work.parent
suite_root = work.parent.parent
identity_file = suite_root / "identity.txt"
if identity_file.exists():
    identity = identity_file.read_text(encoding="utf-8")
    assert identity.endswith("\\nHoroPhysicsTests")
    assert suite_root.name == hashlib.sha256(identity.encode()).hexdigest()[:16]
else:
    assert suite_root.name == "HoroPhysicsTests"
started = time.monotonic()
for directory in (work, temporary, home):
    marker = directory / "shared-fixture"
    assert not marker.exists(), "private case namespace collided"
    marker.write_text(name)
(output / (index + ".ready")).touch()
deadline = time.monotonic() + 5
while len(list(output.glob("*.ready"))) < int(peers):
    assert time.monotonic() < deadline, "isolated cases did not overlap"
    time.sleep(0.01)
time.sleep(0.1)
for directory in (work, temporary, home):
    marker = directory / "shared-fixture"
    assert marker.read_text() == name
    marker.unlink()
(output / (index + ".json")).write_text(json.dumps({"start": started, "end": time.monotonic()}))
'''


def project(tmp_path: Path, *, labels: str = "headless", isolate: bool = True, shared_lock: bool = False,
            peers: int = 1, windows_host: bool = False) -> tuple[Path, Path]:
    source = tmp_path / "source ü"
    source.mkdir()
    build = tmp_path / "build ü"
    evidence = tmp_path / "evidence"
    evidence.mkdir()
    probe = source / "probe.py"
    probe.write_text(PROBE, encoding="utf-8")
    names = [f"HoroPhysicsTests::{name}" for name in NAMES]
    encoded_names = " ".join("[==[" + name.replace(";", "\\;") + "]==]" for name in names)
    registry = [f"set(HoroPhysicsTests_TESTS {encoded_names})"]
    for index, name in enumerate(names):
        registry.append(
            f'add_test([==[{name}]==] "{Path(sys.executable).as_posix()}" "{probe.as_posix()}" '
            f'[==[{name}]==] "{evidence.as_posix()}" {index} {peers})')
        lock = "explicit-shared" if shared_lock else f"explicit-{index}"
        registry.append(f'set_tests_properties([==[{name}]==] PROPERTIES RESOURCE_LOCK {lock})')
    registry_path = source / "registry.cmake"
    registry_path.write_text("\n".join(registry), encoding="utf-8")
    (source / "CMakeLists.txt").write_text("\n".join([
        "cmake_minimum_required(VERSION 3.25)", "project(CaseIsolationContract NONE)", "enable_testing()",
        f'set(CMAKE_HOST_WIN32 {"ON" if windows_host else "${CMAKE_HOST_WIN32}"})',
        f'set(HORO_CI_ISOLATE_TEST_EXECUTABLES {"ON" if isolate else "OFF"} CACHE BOOL "" FORCE)',
        f'include("{(ROOT / "tests/cmake/HoroCiSuites.cmake").as_posix()}")',
        f'set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "{registry_path.as_posix()}")',
        f'set(ARG_LABELS {labels})',
        'set(ARG_PROPERTIES TIMEOUT 10 ENVIRONMENT "PRESERVED_SETTING=present" '
        'ENVIRONMENT_MODIFICATION "EXISTING=set:retained")',
        'horo_ci_catch_properties(HoroPhysicsTests)',
    ]), encoding="utf-8")
    run("cmake", "-S", str(source), "-B", str(build))
    return build, evidence


def manifest(build: Path) -> dict:
    return json.loads(run("ctest", "--test-dir", str(build), "--show-only=json-v1").stdout)


def peak_overlap(evidence: Path) -> int:
    intervals = [json.loads(path.read_text()) for path in evidence.glob("*.json")]
    assert len(intervals) == len(NAMES)
    points = sorted((event[key], delta) for event in intervals for key, delta in (("start", 1), ("end", -1)))
    active = maximum = 0
    for _, delta in points:
        active += delta
        maximum = max(active, maximum)
    assert active == 0
    return maximum


@pytest.mark.parametrize("workers", [2, 3, 4])
def test_case_namespaces_allow_bounded_overlap_and_preserve_properties(tmp_path: Path, workers: int) -> None:
    build, evidence = project(tmp_path, peers=workers)
    first = manifest(build)
    assert first == manifest(build)
    assert {test["name"] for test in first["tests"]} == {f"HoroPhysicsTests::{name}" for name in NAMES}
    for test in first["tests"]:
        properties = {item["name"]: item["value"] for item in test["properties"]}
        case_key = hashlib.sha256(test["name"].encode()).hexdigest()
        assert Path(properties["WORKING_DIRECTORY"]).parent.name == case_key
        assert len(properties["RESOURCE_LOCK"]) == 1
        assert properties["RESOURCE_LOCK"][0].startswith("explicit-")
        assert properties["ENVIRONMENT"] == ["PRESERVED_SETTING=present"]
        assert len(properties["ENVIRONMENT_MODIFICATION"]) == 6
        assert properties["TIMEOUT"] == 10
        assert not properties.get("RUN_SERIAL", False)
    run("ctest", "--test-dir", str(build), "--parallel", str(workers), "--output-on-failure")
    assert peak_overlap(evidence) == workers


def test_case_explicit_shared_resource_lock_still_serializes(tmp_path: Path) -> None:
    build, evidence = project(tmp_path, shared_lock=True)
    run("ctest", "--test-dir", str(build), "--parallel", "4", "--output-on-failure")
    assert peak_overlap(evidence) == 1


@pytest.mark.parametrize("labels", ["gpu", "display", "realtime-safety"])
def test_case_resource_sensitive_suites_retain_suite_lock_and_serial_execution(tmp_path: Path, labels: str) -> None:
    build, _ = project(tmp_path, labels=labels)
    directories = set()
    for test in manifest(build)["tests"]:
        properties = {item["name"]: item["value"] for item in test["properties"]}
        directories.add(properties["WORKING_DIRECTORY"])
        assert "horo-suite-HoroPhysicsTests" in properties["RESOURCE_LOCK"]
        assert properties["RUN_SERIAL"]
    assert len(directories) == 1


def test_case_isolation_is_disabled_outside_ci_mode(tmp_path: Path) -> None:
    build, _ = project(tmp_path, isolate=False)
    for test in manifest(build)["tests"]:
        properties = {item["name"]: item["value"] for item in test["properties"]}
        assert properties["ENVIRONMENT_MODIFICATION"] == ["EXISTING=set:retained"]
        assert all(not lock.startswith("horo-suite-") for lock in properties["RESOURCE_LOCK"])
        assert "ci-runtime" not in Path(properties.get("WORKING_DIRECTORY", "")).parts


def test_case_windows_runner_temp_preserves_build_identity_and_rejects_collisions(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    runner_temp = tmp_path / "runner temp"
    monkeypatch.setenv("RUNNER_TEMP", str(runner_temp))
    build, evidence = project(tmp_path, windows_host=True, peers=4)
    identity = build.resolve().as_posix() + "\nHoroPhysicsTests"
    namespace = runner_temp / "hci" / hashlib.sha256(identity.encode()).hexdigest()[:16]
    identity_file = namespace / "identity.txt"
    assert identity_file.read_text(encoding="utf-8") == identity
    for test in manifest(build)["tests"]:
        properties = {item["name"]: item["value"] for item in test["properties"]}
        assert Path(properties["WORKING_DIRECTORY"]).is_relative_to(namespace)
    run("ctest", "--test-dir", str(build), "--parallel", "4", "--output-on-failure")
    assert peak_overlap(evidence) == 4
    identity_file.write_text("different build", encoding="utf-8")
    with pytest.raises(CalledProcessError) as collision:
        run("cmake", "-S", str(tmp_path / "source ü"), "-B", str(build))
    assert "CI temporary build namespace collision" in collision.value.stdout + collision.value.stderr


@pytest.mark.skipif(sys.platform != "win32", reason="Native Windows classic file-I/O path budget")
def test_case_windows_temp_root_preserves_classic_io_in_deep_save_namespace(tmp_path: Path) -> None:
    source = tmp_path / "source"
    build = tmp_path / "build"
    source.mkdir()
    (source / "fixture.cpp").write_text('''#include <filesystem>
#include <fstream>
#include <iostream>
int main() {
    const auto slots = std::filesystem::temp_directory_path() / "horo-save-root-2" / "approved" / "horo" / "products" /
        "00112233-4455-6677-8899-aabbccddeeff" / "00000000-0000-0000-0000-000000000002" / "server" /
        "00000000-0000-0000-0000-000000000003" / "slots";
    const auto path = slots / ".namespace.lock";
    if (path.native().size() >= 260) {
        std::cerr << "Classic fixture path exceeds MAX_PATH: " << path.native().size();
        return 1;
    }
    std::filesystem::create_directories(slots);
    { std::ofstream output(path, std::ios::binary); output << "owned"; if (!output.good()) return 2; }
    { std::ifstream input(path, std::ios::binary); if (!input.good()) return 3; }
    const auto moved = slots / ".moved.lock";
    std::filesystem::rename(path, moved);
    return std::filesystem::remove(moved) ? 0 : 4;
}
''')
    target = "HoroRuntimeSaveFilesystemLockTests"
    (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.25)
project(WindowsFixturePathBudget CXX)
enable_testing()
add_executable({target} fixture.cpp)
target_compile_features({target} PRIVATE cxx_std_20)
set(HORO_CI_ISOLATE_TEST_EXECUTABLES ON CACHE BOOL "" FORCE)
include("{(ROOT / 'tests/cmake/HoroCiSuites.cmake').as_posix()}")
file(GENERATE OUTPUT "${{CMAKE_CURRENT_BINARY_DIR}}/registry.cmake" CONTENT
    "set({target}_TESTS classic)\\nadd_test(classic \\"$<TARGET_FILE:{target}>\\")\\n")
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${{CMAKE_CURRENT_BINARY_DIR}}/registry.cmake")
set(ARG_LABELS headless)
set(ARG_PROPERTIES TIMEOUT 10)
horo_ci_catch_properties({target})
''')
    run("cmake", "-S", str(source), "-B", str(build), "-G", "Ninja")
    run("cmake", "--build", str(build), "--parallel", "1")
    run("ctest", "--test-dir", str(build), "--output-on-failure")


@pytest.mark.skipif(sys.platform != "linux" or not shutil.which("gcc") or not shutil.which("gcov"),
                    reason="Linux GCC/gcov coverage merge contract")
def test_case_parallel_coverage_preserves_full_counters(tmp_path: Path) -> None:
    source = tmp_path / "source"
    build = tmp_path / "build"
    source.mkdir()
    (source / "fixture.c").write_text('''#include <unistd.h>
int untouched(int value) {
    return value + 42;
}
int branch(int mode) {
    if (mode) return 1;
    return 0;
}
int main(int argc, char **argv) {
    usleep(100000);
    return branch(argv[1][0] == '1') < 0;
}
''')
    registrations = ["set(HoroPhysicsTests_TESTS case0 case1 case2 case3)"]
    registrations.extend(
        f'add_test(case{index} "$<TARGET_FILE:HoroPhysicsTests>" {index % 2})' for index in range(4))
    registry = "\n".join(registrations)
    (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.25)
project(CaseCoverageContract C)
enable_testing()
add_executable(HoroPhysicsTests fixture.c)
target_compile_options(HoroPhysicsTests PRIVATE --coverage -fprofile-abs-path)
target_link_options(HoroPhysicsTests PRIVATE --coverage)
set(HORO_CI_ISOLATE_TEST_EXECUTABLES ON CACHE BOOL "" FORCE)
include("{(ROOT / 'tests/cmake/HoroCiSuites.cmake').as_posix()}")
file(GENERATE OUTPUT "${{CMAKE_CURRENT_BINARY_DIR}}/registry.cmake" CONTENT [==[{registry}]==])
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${{CMAKE_CURRENT_BINARY_DIR}}/registry.cmake")
set(ARG_LABELS headless)
set(ARG_PROPERTIES TIMEOUT 10)
horo_ci_catch_properties(HoroPhysicsTests)
''')
    run("cmake", "-S", str(source), "-B", str(build), "-DCMAKE_C_COMPILER=gcc")
    run("cmake", "--build", str(build), "--parallel", "1")
    notes = list(build.rglob("*.gcno"))
    assert len(notes) == 1
    reports = []
    for workers in (1, 4):
        run("ctest", "--test-dir", str(build), "--parallel", str(workers), "--output-on-failure")
        run("gcov", "--json-format", str(notes[0]), cwd=build)
        documents = list(build.glob("*.gcov.json.gz"))
        assert len(documents) == 1
        reports.append(json.loads(gzip.decompress(documents[0].read_bytes()))["files"])
        counters = list(build.rglob("*.gcda"))
        assert len(counters) == 1 and not counters[0].is_symlink()
        counters[0].unlink()
    assert reports[0] == reports[1]
    counts = [line["count"] for file in reports[1] for line in file["lines"]]
    assert 0 in counts and any(count > 0 for count in counts)
