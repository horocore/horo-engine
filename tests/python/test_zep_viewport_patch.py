"""Exercise Zep patch preparation through the actual CMake helper."""

from pathlib import Path
import shutil
import subprocess

import pytest


@pytest.mark.parametrize("line_ending", [b"\n", b"\r\n"])
@pytest.mark.parametrize("autocrlf", ["false", "true"])
def test_zep_patch_normalizes_checkout_and_preserves_rejection(
        tmp_path: Path, line_ending: bytes, autocrlf: str, monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("GIT_CONFIG_COUNT", "1")
    monkeypatch.setenv("GIT_CONFIG_KEY_0", "core.autocrlf")
    monkeypatch.setenv("GIT_CONFIG_VALUE_0", autocrlf)
    cmake = shutil.which("cmake")
    git = shutil.which("git")
    assert cmake is not None
    assert git is not None
    source = tmp_path / "source with space ü"
    source.mkdir()
    subprocess.run([git, "init", "-q", str(source)], check=True, capture_output=True, timeout=15)
    original = b"\nint viewport = 0;\n// tail\n"
    (source / "viewport.cpp").write_bytes(original.replace(b"\n", b"\r\n") if autocrlf == "true" else original)
    helper = tmp_path / "cmake/ApplyZepViewportPatch.cmake"
    helper.parent.mkdir()
    shutil.copyfile(Path(__file__).resolve().parents[2] / "cmake/ApplyZepViewportPatch.cmake", helper)
    patch = (b"--- a/viewport.cpp\n+++ b/viewport.cpp\n@@ -1,3 +1,4 @@\n\n"
             b" int viewport = 0;\n+// bridge @literal@ ${literal} \\n\n // tail\n")
    patch_dir = helper.parent / "patches/zep"
    patch_dir.mkdir(parents=True)
    patch_path = patch_dir / "horo-viewport.patch"
    checkout_patch = patch.replace(b"\n", line_ending)
    patch_path.write_bytes(checkout_patch)
    script = tmp_path / "run.cmake"
    script.write_text(f'set(zep_SOURCE_DIR "{source.as_posix()}")\n'
                      f'include("{helper.as_posix()}")\n', encoding="utf-8")
    for _ in range(2):
        result = subprocess.run([cmake, "-P", str(script)], cwd=tmp_path, capture_output=True,
                                text=True, encoding="utf-8", timeout=15)
        assert result.returncode == 0, result.stderr
        assert (tmp_path / "generated/zep/horo-viewport.patch").read_bytes() == patch
        assert patch_path.read_bytes() == checkout_patch
    patched = (source / "viewport.cpp").read_bytes()
    assert b"// bridge @literal@ ${literal} \\n" in patched
    (source / "viewport.cpp").write_bytes(b"incompatible source\n")
    rejected = subprocess.run([cmake, "-P", str(script)], cwd=tmp_path, capture_output=True,
                              text=True, encoding="utf-8", timeout=15)
    assert rejected.returncode != 0
    assert "Pinned Zep source does not match" in rejected.stderr
    assert "viewport.cpp" in rejected.stderr
    assert (source / "viewport.cpp").read_bytes() == b"incompatible source\n"
