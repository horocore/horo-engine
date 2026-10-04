"""Run the reviewed patch on real dependency sources, including Windows text writes."""
import importlib.util
from pathlib import Path
import re
import shutil
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("horo_jolt_patch_runner", ROOT / "scripts/dev.py")
assert SPEC is not None
assert SPEC.loader is not None
dev = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = dev
SPEC.loader.exec_module(dev)
PATCH = ROOT / "cmake/patches/HoroJoltConstraintReservation.cmake"
FILES = ("Constraints/ConstraintManager.h", "Constraints/ConstraintManager.cpp", "PhysicsSystem.h")


@pytest.fixture
def source(tmp_path):
    candidates = [ROOT / ".cmake/fetchcontent/horo_jolt-src"]
    candidates.extend((ROOT / "build").glob("*/_deps/horo_jolt-src"))
    upstream = next((path for path in candidates if (path / "Jolt/Physics/PhysicsSystem.h").is_file()), None)
    if upstream is None:
        pytest.skip("Canonical Physics dependency must be populated before this test")
    for relative in FILES:
        target = tmp_path / "Jolt/Physics" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(upstream / "Jolt/Physics" / relative, target)
    return tmp_path


def apply(source):
    cmake = shutil.which("cmake")
    assert cmake is not None
    return dev.execute_subprocess(
        [str(Path(cmake).resolve()), f"-DJOLT_SOURCE_DIR={source}", "-P", str(PATCH)], cwd=ROOT,
    )


@pytest.mark.parametrize("fresh", [False, True])
@pytest.mark.parametrize("crlf", [False, True])
def test_patch_accepts_native_line_endings_and_is_idempotent(source, crlf, fresh):
    if fresh:
        # Recover upstream text from the reviewed patched dependency. The patch's
        # original digests must accept it before this test can succeed.
        for relative in FILES:
            path = source / "Jolt/Physics" / relative
            original = re.sub(
                r"\t/// Horo patch:[^\n]+\n(?:\tuint32 [^\n]+\n)+\n", "", path.read_text(),
            )
            original = re.sub(
                r"// Horo patch: capacity-only operations[^\n]+\n.*?(?=void ConstraintManager::Add)",
                "", original, flags=re.S,
            )
            path.write_bytes(original.encode().replace(b"\n", b"\r\n") if crlf else original.encode())
    assert apply(source) == 0
    expected = {}
    for relative in FILES:
        path = source / "Jolt/Physics" / relative
        expected[relative] = path.read_bytes().replace(b"\r\n", b"\n")
        path.write_bytes(expected[relative].replace(b"\n", b"\r\n") if crlf else expected[relative])
    for _ in range(2):
        result = apply(source)
        assert result == 0
        for relative in FILES:
            assert (source / "Jolt/Physics" / relative).read_bytes().replace(b"\r\n", b"\n") == expected[relative]


@pytest.mark.parametrize("relative", FILES)
def test_patch_rejects_content_drift_before_writing_any_file(source, relative):
    path = source / "Jolt/Physics" / relative
    path.write_bytes(path.read_bytes() + b"\n// unreviewed drift\n")
    before = {name: (source / "Jolt/Physics" / name).read_bytes() for name in FILES}
    result = apply(source)
    assert result != 0
    assert {name: (source / "Jolt/Physics" / name).read_bytes() for name in FILES} == before
