"""Keep Jolt debug information while avoiding contradictory cached MSVC flags."""
from pathlib import Path

import pytest

from test_ci_test_isolation import run

ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize("msvc,launcher,format_name,expected", [
    ("TRUE", "sccache", "Embedded", "OFF"),
    ("TRUE", "cl", "Embedded", "ON"),
    ("TRUE", "sccache", "ProgramDatabase", "ON"),
    ("FALSE", "ccache", "Embedded", "ON"),
])
def test_real_physics_recipe_debug_format_guard(tmp_path, msvc, launcher, format_name, expected):
    source = tmp_path / "source with spaces ü"
    source.mkdir()
    recipe = (ROOT / "cmake/HoroPhysicsDependency.cmake").as_posix()
    (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.25)
project(JoltDebugFormat NONE)
include("{recipe}")
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(MSVC {msvc})
set(CMAKE_CXX_COMPILER_LAUNCHER "{launcher}")
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "{format_name}")
function(FetchContent_Declare)
endfunction()
# Observe the actual recipe at its upstream option boundary, without fetching
# or compiling Jolt. The macro returns from the owning recipe function before
# the independent source-patch/license verification paths.
macro(FetchContent_MakeAvailable)
    file(WRITE "${{CMAKE_BINARY_DIR}}/upstream-format.txt"
        "${{GENERATE_DEBUG_SYMBOLS}};${{CMAKE_MSVC_DEBUG_INFORMATION_FORMAT}}")
    return()
endmacro()
horo_add_canonical_physics_dependency()
if(NOT CMAKE_MSVC_DEBUG_INFORMATION_FORMAT STREQUAL "{format_name}")
    message(FATAL_ERROR "Recipe changed the caller's debug format")
endif()
''', encoding="utf-8")
    build = tmp_path / "build with spaces ü"
    run("cmake", "-S", str(source), "-B", str(build))
    assert (build / "upstream-format.txt").read_text(encoding="utf-8") == f"{expected};{format_name}"
