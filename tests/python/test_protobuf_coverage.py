"""CMake scope contracts; the compiler regression requires native GCC 15."""

from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize(
    "compiler,version,coverage,excluded",
    [
        ("GNU", "15.2.0", "ON", True),
        ("GNU", "14.3.0", "ON", False),
        ("GNU", "15.2.0", "OFF", False),
        ("Clang", "20.1.0", "ON", False),
        ("AppleClang", "17.0.0", "ON", False),
        ("MSVC", "19.44", "ON", False),
    ],
)
def test_protobuf_coverage_is_private_and_compiler_scoped(
    tmp_path, compiler, version, coverage, excluded
):
    cmake = shutil.which("cmake")
    assert cmake, "CMake is required for the coverage scope contract"
    module = (ROOT / "cmake/HoroProtobufCoverage.cmake").as_posix()
    (tmp_path / "CMakeLists.txt").write_text(
        f'''cmake_minimum_required(VERSION 3.25)
project(ProtobufCoverageScope NONE)
# Configure-only compiler models: this fixture must never be built.
set(CMAKE_CXX_CREATE_STATIC_LIBRARY "scope-contract-not-built")
set(CMAKE_CXX_COMPILER_ID "{compiler}")
set(CMAKE_CXX_COMPILER_VERSION "{version}")
set(HORO_CI_SONAR_COVERAGE {coverage})
set(CMAKE_CXX_FLAGS_DEBUG "--coverage -fprofile-abs-path")
foreach(target libprotobuf libprotobuf-lite product)
    add_library(${{target}} STATIC dummy.cc)
    set_target_properties(${{target}} PROPERTIES LINKER_LANGUAGE CXX)
endforeach()
add_library(protobuf::libprotobuf ALIAS libprotobuf)
target_link_libraries(product PRIVATE protobuf::libprotobuf)
include("{module}" OPTIONAL)
foreach(target libprotobuf libprotobuf-lite product)
    get_target_property(options ${{target}} COMPILE_OPTIONS)
    get_target_property(interface ${{target}} INTERFACE_COMPILE_OPTIONS)
    file(WRITE "${{CMAKE_BINARY_DIR}}/${{target}}.txt" "${{options}}\n${{interface}}")
endforeach()
file(WRITE "${{CMAKE_BINARY_DIR}}/flags.txt" "${{CMAKE_CXX_FLAGS_DEBUG}}")
''',
        encoding="utf-8",
    )
    (tmp_path / "dummy.cc").write_text("int value() { return 1; }\n", encoding="utf-8")
    build = tmp_path / "build"
    result = subprocess.run(
        [cmake, "-S", str(tmp_path), "-B", str(build)],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    for target in ("libprotobuf", "libprotobuf-lite", "product"):
        options, interface = (build / f"{target}.txt").read_text().splitlines()
        expected = "-fprofile-exclude-files=.*" if excluded and target != "product" else "options-NOTFOUND"
        assert options == expected
        assert interface == "interface-NOTFOUND"
    assert (build / "flags.txt").read_text() == "--coverage -fprofile-abs-path"


def test_protobuf_policy_is_applied_after_upstream_target_creation():
    dependencies = (ROOT / "cmake/Dependencies.cmake").read_text(encoding="utf-8")
    creation = 'add_subdirectory("${horo_protobuf_SOURCE_DIR}" "${horo_protobuf_BINARY_DIR}")'
    assert creation + "\n    include(HoroProtobufCoverage)" in dependencies
