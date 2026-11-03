"""Check content-based module binding identities without compiling the engine."""

from pathlib import Path
import re
import shutil
import subprocess

import tempfile
import unittest


PROJECT_ROOT = Path(__file__).resolve().parents[2]


def _run(*command: str) -> None:
    completed = subprocess.run(command, capture_output=True, text=True, timeout=45, check=False)
    assert completed.returncode == 0, completed.stdout + completed.stderr


def _fixture(source: Path, module: Path) -> None:
    names = set(re.findall(r"(?:src|include)/(?:[A-Za-z0-9_.-]+/)*[A-Za-z0-9_.-]+\.(?:cpp|h)", module.read_text()))
    names.update({"src/extensions/script/IdentityFixture.cpp", "include/Horo/Extensions/IdentityFixture.h"})
    for name in names:
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("// Configure-only identity fixture.\n")
    (source / "probe.cpp").write_text("int identity_probe;\n")
    (source / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.25)\n"
        "project(ModuleIdentityRegression LANGUAGES CXX)\n"
        f'include("{module.as_posix()}")\n'
        "add_library(IdentityProbe STATIC EXCLUDE_FROM_ALL probe.cpp)\n"
        f"horo_configure_script_module_identity(IdentityProbe FIXTURE_ARCHIVE_SHA256 {'a' * 64})\n"
        'file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/identity.txt" '
        'CONTENT "$<TARGET_PROPERTY:IdentityProbe,COMPILE_DEFINITIONS>")\n'
    )


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "CMake and Ninja are required")
class ScriptModuleIdentityTests(unittest.TestCase):
    def test_content_membership_and_configuration(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="horo-module-identity-")
        self.addCleanup(temporary.cleanup)
        tmp_path = Path(temporary.name)
        source, build = tmp_path / "source", tmp_path / "build"
        source.mkdir()
        _fixture(source, PROJECT_ROOT / "cmake/HoroScriptModuleIdentity.cmake")
        _run("cmake", "-S", str(source), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug")
        identity = build / "identity.txt"
        initial = identity.read_text()
        _run("cmake", "--build", str(build), "--parallel", "1")
        assert identity.read_text() == initial

        changed = source / "src/extensions/host/ModuleScriptNativePort.cpp"
        changed.write_text(changed.read_text() + "// Tracked binding changed.\n")
        _run("cmake", "--build", str(build), "--parallel", "1")
        edited = identity.read_text()
        assert edited != initial

        (source / "src/extensions/script/NewBindingFixture.cpp").write_text("int new_binding;\n")
        _run("cmake", "--build", str(build), "--parallel", "1")
        added = identity.read_text()
        assert added != edited

        (source / "unrelated.txt").write_text("Unrelated metadata.\n")
        _run("cmake", "--build", str(build), "--parallel", "1")
        assert identity.read_text() == added
        _run("cmake", "-S", str(source), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release")
        assert identity.read_text() != added
        assert ":Release" in identity.read_text()
        assert not list(build.rglob("*.o"))
        assert not list(build.rglob("*.obj"))


if __name__ == "__main__":
    unittest.main()
