"""Exercise the actual owned-copy helper without configuring or compiling GNS."""
import hashlib
import json
from pathlib import Path
import shutil

import pytest


@pytest.mark.parametrize("inside_repository", [True, False])
def test_gns_patch_stays_in_owned_copy(tmp_path: Path, inside_repository: bool,
                                      pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch):
    project = tmp_path / "project with space ü"
    project.mkdir()
    git = shutil.which("git")
    cmake = shutil.which("cmake")
    assert git is not None
    assert cmake is not None
    git, cmake = [str(Path(tool).resolve(strict=True)) for tool in (git, cmake)]
    monkeypatch.chdir(project)
    initialized = pytester.run(git, "init", "-q", str(project), timeout=15)
    assert initialized.ret == 0, initialized.stdout.str() + initialized.stderr.str()
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
        result = pytester.run(cmake, "-P", str(script), timeout=15)
        assert result.ret == 0, result.stdout.str() + result.stderr.str()
        owned = Path(result_path.read_text())
        assert (owned / "src/sentinel.cpp").read_text() == "patched\n"
        assert parent_source.read_text() == "original\n"
        assert (pristine / "src/sentinel.cpp").read_text() == "original\n"
    (owned / "src/sentinel.cpp").write_text("corrupted\n", encoding="utf-8")
    rejected = pytester.run(cmake, "-P", str(script), timeout=15)
    assert rejected.ret != 0
    assert "source changed" in rejected.stderr.str()
    assert parent_source.read_text() == "original\n"
    assert (pristine / "src/sentinel.cpp").read_text() == "original\n"
