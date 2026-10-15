"""Real multi-module CMake generation preserves primary freshness ownership."""
from __future__ import annotations

from pathlib import Path
import subprocess
import shutil
import sys

ROOT = Path(__file__).resolve().parents[2]


def test_additional_module_owns_distinct_inputs_without_changing_primary(tmp_path: Path) -> None:
    tmp_path = tmp_path / "Project with spaces"
    tmp_path.mkdir()
    for name in ("Primary", "Secondary"):
        (tmp_path / f"{name}.cpp").write_text(f"int {name.lower()}_fixture;\n", encoding="utf-8")
    cmake = """cmake_minimum_required(VERSION 3.25)
project(MultipleGameplayModules LANGUAGES CXX)
add_library(GameplayApi INTERFACE)
add_library(HoroEngine::GameplayApi ALIAS GameplayApi)
set(HORO_GAMEPLAY_SDK_FINGERPRINT fixture-sdk)
include("@HELPER@")
horo_add_gameplay_module(Primary MODULE_ID game.primary NO_MANIFEST SOURCES Primary.cpp)
horo_add_gameplay_module(Secondary MODULE_ID game.secondary NO_MANIFEST
    INPUT_MANIFEST_OUTPUT "${CMAKE_BINARY_DIR}/Secondary/inputs.txt" SOURCES Secondary.cpp)
get_property(primary_revision TARGET Primary PROPERTY HORO_GAMEPLAY_DESCRIPTOR_REVISION_FILE)
get_property(secondary_revision TARGET Secondary PROPERTY HORO_GAMEPLAY_DESCRIPTOR_REVISION_FILE)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/revisions.txt"
    CONTENT "${primary_revision}\n${secondary_revision}\n")
""".replace("@HELPER@", (ROOT / "cmake/HoroGameplayModule.cmake").as_posix())
    (tmp_path / "CMakeLists.txt").write_text(cmake, encoding="utf-8")
    build = tmp_path / "build"
    cmake_executable = shutil.which("cmake")
    assert cmake_executable is not None, "CMake is required for the real module configuration regression"
    cmake_executable = str(Path(cmake_executable).resolve(strict=True))
    configured = subprocess.run(
        [cmake_executable, "-S", str(tmp_path), "-B", str(build), "-G", "Ninja",
         f"-DPython3_EXECUTABLE={sys.executable}"],
        check=False, shell=False, capture_output=True, text=True,
    )
    assert configured.returncode == 0, configured.stdout + configured.stderr
    assert (tmp_path / ".horo/local/gameplay_build_inputs.txt").read_text(encoding="utf-8") == "Primary.cpp\n"
    assert (build / "Secondary/inputs.txt").read_text(encoding="utf-8") == "Secondary.cpp\n"
    revisions = (build / "revisions.txt").read_text(encoding="utf-8").splitlines()
    assert len(revisions) == len(set(revisions)) == 2
    for name, revision in zip(("Primary", "Secondary"), revisions, strict=True):
        assert Path(revision) == build / f"{name}_generated_behavior_bundle.revision"
        graph = (build / "build.ninja").read_text(encoding="utf-8")
        assert f"{name}_generated_behavior_bundle.cpp" in graph
        assert f"--module-id game.{name.lower()} --fingerprint fixture-sdk" in graph
