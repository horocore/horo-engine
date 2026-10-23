"""Exercise the actual owned-copy helper without configuring or compiling GNS."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

import pytest


@pytest.mark.parametrize("inside_repository", [True, False])
def test_gns_patch_stays_in_owned_copy(tmp_path: Path, inside_repository: bool):
    project = tmp_path / "project with space ü"
    project.mkdir()
    subprocess.run(["git", "init", "-q", str(project)], check=True)
    parent_source = project / "src/sentinel.cpp"
    parent_source.parent.mkdir()
    parent_source.write_text("original\n", encoding="utf-8")
    pristine = project / "pristine"
    (pristine / "src").mkdir(parents=True)
    (pristine / "src/sentinel.cpp").write_text("original\n", encoding="utf-8")
    (pristine / "LICENSE").write_text("fixture license\n", encoding="utf-8")
    helper = project / "cmake/HoroGnsSelectedRoot.cmake"
    helper.parent.mkdir()
    shutil.copyfile(Path(__file__).resolve().parents[2] / "cmake/HoroGnsSelectedRoot.cmake", helper)
    patch_dir = helper.parent / "patches/gns"
    patch_dir.mkdir(parents=True)
    patch = "--- a/src/sentinel.cpp\n+++ b/src/sentinel.cpp\n@@ -1 +1 @@\n-original\n+patched\n"
    (patch_dir / "horo-gns-selected-root-v1.patch").write_text(patch, encoding="utf-8")
    digest = lambda text: hashlib.sha256(text.encode()).hexdigest()
    manifest = {"patchSha256": digest(patch), "licenseSha256": digest("fixture license\n"),
                "paths": [{"path": "src/sentinel.cpp", "pristineSha256": digest("original\n"),
                           "candidateSha256": digest("patched\n")} ]}
    (patch_dir / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    build = project / "build" if inside_repository else tmp_path / "external build"
    script = tmp_path / "run.cmake"
    result_path = tmp_path / "result.txt"
    script.write_text(f"set(CMAKE_BINARY_DIR [==[{build}]==])\ninclude([==[{helper}]==])\n"
                      f"horo_prepare_gns_selected_root([==[{pristine}]==] patched)\n"
                      f"file(WRITE [==[{result_path}]==] \"${{patched}}\")\n", encoding="utf-8")
    for _ in range(2):
        result = subprocess.run(["cmake", "-P", str(script)], cwd=project, capture_output=True, text=True, timeout=15)
        assert result.returncode == 0, result.stdout + result.stderr
        owned = Path(result_path.read_text())
        assert (owned / "src/sentinel.cpp").read_text() == "patched\n"
        assert parent_source.read_text() == "original\n"
        assert (pristine / "src/sentinel.cpp").read_text() == "original\n"
    (owned / "src/sentinel.cpp").write_text("corrupted\n", encoding="utf-8")
    rejected = subprocess.run(["cmake", "-P", str(script)], cwd=project, capture_output=True, text=True, timeout=15)
    assert rejected.returncode != 0
    assert "source changed" in rejected.stderr
    assert parent_source.read_text() == "original\n"
    assert (pristine / "src/sentinel.cpp").read_text() == "original\n"
