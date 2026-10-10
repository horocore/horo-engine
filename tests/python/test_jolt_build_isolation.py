"""Qualify Jolt build isolation without compiling or fetching the dependency."""

from pathlib import Path
import re
import shutil
# This compiler-free contract executes only the installed CMake tool.
import subprocess  # nosec B404


ROOT = Path(__file__).resolve().parents[2]


def test_release_configuration_preserves_debug_jolt_artifacts(tmp_path):
    cmake = shutil.which("cmake")
    assert cmake is not None
    dependency = (ROOT / "cmake/HoroPhysicsDependency.cmake").read_text()
    declaration = re.search(r"    FetchContent_Declare\(horo_jolt\n.*?\n    \)", dependency, re.S)
    assert declaration is not None

    # Exercise the production declaration with a local, compiler-free stand-in.
    # A configuration marker models the archive overwritten by the second preset.
    upstream = tmp_path / "upstream"
    (upstream / "Build").mkdir(parents=True)
    (upstream / "Build/CMakeLists.txt").write_text(
        'file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/archive-config.txt" "${CMAKE_BUILD_TYPE}")\n'
    )
    source = tmp_path / "source"
    source.mkdir()
    (source / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.25)\n"
        "project(JoltBuildIsolation LANGUAGES NONE)\n"
        "include(FetchContent)\n"
        + declaration.group()
        + '\nFetchContent_MakeAvailable(horo_jolt)\n'
        'file(WRITE "${CMAKE_BINARY_DIR}/dependency-build.txt" "${horo_jolt_BINARY_DIR}")\n'
    )
    builds = {}
    for configuration in ("Debug", "Release"):
        build = tmp_path / configuration
        # shutil.which verified the installed executable; argv uses fixed settings
        # and test-owned paths with shell=False and a bounded timeout.
        # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit
        subprocess.run(  # nosec B603
            [cmake, "-S", str(source), "-B", str(build),
             f"-DCMAKE_BUILD_TYPE={configuration}",
             f"-DFETCHCONTENT_BASE_DIR={tmp_path / 'shared-fetchcontent'}",
             f"-DFETCHCONTENT_SOURCE_DIR_HORO_JOLT={upstream}"],
            check=True, capture_output=True, text=True, timeout=30, shell=False,
        )
        builds[configuration] = Path((build / "dependency-build.txt").read_text())

    assert builds["Debug"] != builds["Release"]
    for configuration, build in builds.items():
        assert (build / "archive-config.txt").read_text() == configuration
