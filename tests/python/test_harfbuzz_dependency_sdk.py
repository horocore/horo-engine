"""Exercise the real HarfBuzz recipe with a tiny offline upstream fixture."""

from pathlib import Path
import shutil

import pytest

from test_ci_test_isolation import run


ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture
def harfbuzz_project(tmp_path):
    if not shutil.which("cmake"):
        pytest.skip("CMake unavailable")
    upstream = tmp_path / "upstream source ü"
    (upstream / "src").mkdir(parents=True)
    (upstream / "src/hb.h").write_text("int hb_fixture_probe();\n", encoding="utf-8")
    (upstream / "src/probe.cpp").write_text(
        '#include "hb.h"\nint hb_fixture_probe() { return 42; }\n', encoding="utf-8")
    (upstream / "COPYING").write_text("Fixture license\n", encoding="utf-8")
    (upstream / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.20)
project(HarfBuzzFixture CXX)
set(project_headers "${CMAKE_CURRENT_SOURCE_DIR}/src/hb.h")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/src/hb-features.h" "#define HB_FIXTURE 1\\n")
file(WRITE "${CMAKE_BINARY_DIR}/source-configured.txt" "source")
add_library(harfbuzz STATIC src/probe.cpp)
target_include_directories(harfbuzz PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
''', encoding="utf-8")

    host = tmp_path / "host source ü"
    host.mkdir()
    (host / "probe.cpp").write_text(
        '#include <hb.h>\nint main() { return hb_fixture_probe() != 42; }\n', encoding="utf-8")
    (host / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.20)
project(HarfBuzzSdkHost C CXX)
set(CMAKE_CXX_STANDARD 20)
list(APPEND CMAKE_MODULE_PATH "@MODULE_DIR@")
include(HoroHarfBuzzDependency)
horo_add_harfbuzz_dependency()
export(TARGETS harfbuzz NAMESPACE HoroEngine:: FILE "${CMAKE_BINARY_DIR}/HarfBuzzTargets.cmake")
add_executable(probe probe.cpp)
target_link_libraries(probe PRIVATE harfbuzz)
enable_testing()
add_test(NAME harfbuzz-probe COMMAND probe)
'''.replace("@MODULE_DIR@", (ROOT / "cmake").as_posix()), encoding="utf-8")

    def configure(name, *options):
        build = tmp_path / name
        run("cmake", "-S", str(host), "-B", str(build), "-DCMAKE_BUILD_TYPE=Debug",
            f"-DFETCHCONTENT_SOURCE_DIR_HARFBUZZ={upstream}",
            "-DHORO_BUILD_DEPENDENCY_SDK_PACKAGES=ON", *options)
        return build

    producer = configure("producer ü")
    run("cmake", "--build", str(producer), "--config", "Debug",
        "--target", "horo-package-dependencies", "--parallel", "1")
    packages = producer / "dependency-sdk/packages"
    archive, = packages.glob("HarfBuzz-*.tar.gz")
    return configure, producer, packages, archive


def assert_probe(build):
    run("cmake", "--build", str(build), "--config", "Debug", "--target", "probe", "--parallel", "1")
    run("ctest", "--test-dir", str(build), "-C", "Debug", "--output-on-failure", "--no-tests=error")


def test_verified_package_avoids_upstream_and_exports_to_gameplay(harfbuzz_project, tmp_path):
    configure, producer, packages, _ = harfbuzz_project
    assert (producer / "source-configured.txt").is_file()
    build = configure("consumer ü", f"-DHORO_DEPENDENCY_SDK_CACHE={packages}")
    assert not (build / "source-configured.txt").exists()
    assert_probe(build)
    restored, = (build / "dependency-sdk/restored").iterdir()
    assert (restored / "include/hb-features.h").is_file()
    assert (restored / "share/COPYING").read_text(encoding="utf-8") == "Fixture license\n"

    external = tmp_path / "external gameplay ü"
    external.mkdir()
    (external / "main.cpp").write_text(
        '#include <hb.h>\nint main() { return hb_fixture_probe() != 42; }\n', encoding="utf-8")
    (external / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.20)
project(ExternalHarfBuzz CXX)
find_package(Threads REQUIRED)
include("${SDK_EXPORT}")
add_executable(probe main.cpp)
target_link_libraries(probe PRIVATE HoroEngine::harfbuzz)
enable_testing()
add_test(NAME harfbuzz-probe COMMAND probe)
''', encoding="utf-8")
    external_build = external / "build"
    run("cmake", "-S", str(external), "-B", str(external_build),
        f"-DSDK_EXPORT={build / 'HarfBuzzTargets.cmake'}")
    assert_probe(external_build)


def test_corrupt_package_falls_back_to_source(harfbuzz_project):
    configure, _, packages, archive = harfbuzz_project
    archive.write_bytes(b"corrupt SDK")
    build = configure("corrupt fallback", f"-DHORO_DEPENDENCY_SDK_CACHE={packages}")
    assert (build / "source-configured.txt").is_file()
    assert_probe(build)


@pytest.mark.parametrize("option", ["HB_HAVE_DIRECTWRITE", "HB_HAVE_UNISCRIBE", "HB_HAVE_GDI",
                                   "HB_HAVE_INTROSPECTION", "BUILD_SHARED_LIBS", "HORO_CI_SONAR_COVERAGE"])
def test_custom_or_instrumented_composition_stays_on_source(harfbuzz_project, option):
    configure, _, packages, _ = harfbuzz_project
    build = configure(option, f"-DHORO_DEPENDENCY_SDK_CACHE={packages}", f"-D{option}=ON")
    assert (build / "source-configured.txt").is_file()
    assert not (build / "dependency-sdk/restored").exists()
    assert_probe(build)
