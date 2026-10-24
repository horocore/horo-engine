"""Exercise the actual owned-copy helper without configuring or compiling GNS."""
from collections.abc import Iterator
import hashlib
import json
from pathlib import Path
import shutil

import pytest


ORIGINAL_SOURCE = "original @preserved@ ${preserved}\n"
PATCHED_SOURCE = "patched @preserved@ ${preserved}\n"


def _prepare_patch_repository(project: Path, patch_path: Path, git: str,
                              pytester: pytest.Pytester, autocrlf_checkout: bool) -> None:
    """Keep checkout conversion inside an isolated index using the real attribute."""
    initialized = pytester.run(git, "init", "-q", str(project), timeout=15)
    assert initialized.ret == 0, initialized.stdout.str() + initialized.stderr.str()
    if not autocrlf_checkout:
        return
    shutil.copyfile(Path(__file__).resolve().parents[2] / ".gitattributes", project / ".gitattributes")
    patch_relative = patch_path.relative_to(project).as_posix()
    expected = patch_path.read_bytes()
    staged = pytester.run(git, "-c", "core.autocrlf=false", "add", "--",
                          ".gitattributes", patch_relative, timeout=15)
    assert staged.ret == 0, staged.stdout.str() + staged.stderr.str()
    patch_path.unlink()
    checked_out = pytester.run(git, "-c", "core.autocrlf=true", "checkout", "--", patch_relative, timeout=15)
    assert checked_out.ret == 0, checked_out.stdout.str() + checked_out.stderr.str()
    assert patch_path.read_bytes() == expected


@pytest.fixture(params=[False, True])
def isolated_git_config(tmp_path: Path, request: pytest.FixtureRequest,
                        monkeypatch: pytest.MonkeyPatch) -> Iterator[None]:
    """Keep inherited checkout conversion independent of caller configuration."""
    git_config = tmp_path / "isolated.gitconfig"
    config_text = f"[core]\n    autocrlf = {str(request.param).lower()}\n"
    git_config.write_text(config_text, encoding="utf-8")
    monkeypatch.setenv("GIT_CONFIG_GLOBAL", str(git_config))
    monkeypatch.setenv("GIT_CONFIG_NOSYSTEM", "1")
    monkeypatch.delenv("GIT_CONFIG_COUNT", raising=False)
    yield
    assert git_config.read_text(encoding="utf-8") == config_text


@pytest.mark.parametrize("pristine_newline", [b"\n", b"\r\n"])
@pytest.mark.usefixtures("isolated_git_config")
@pytest.mark.parametrize("inside_repository", [True, False])
@pytest.mark.parametrize("autocrlf_checkout", [False, True])
def test_gns_patch_stays_in_owned_copy(tmp_path: Path, inside_repository: bool, autocrlf_checkout: bool,
                                      pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch, pristine_newline: bytes):
    project = tmp_path / "project with space ü"
    project.mkdir()
    git = shutil.which("git")
    cmake = shutil.which("cmake")
    assert git is not None
    assert cmake is not None
    git, cmake = [str(Path(tool).resolve(strict=True)) for tool in (git, cmake)]
    monkeypatch.chdir(project)
    parent_source = project / "src/sentinel.cpp"
    parent_source.parent.mkdir()
    parent_source.write_bytes(ORIGINAL_SOURCE.encode().replace(b"\n", pristine_newline))
    pristine = project / "pristine"
    (pristine / "src").mkdir(parents=True)
    (pristine / "src/sentinel.cpp").write_bytes(ORIGINAL_SOURCE.encode().replace(b"\n", pristine_newline))
    (pristine / "LICENSE").write_text("fixture license\n", encoding="utf-8")
    helper = project / "cmake/HoroGnsSelectedRoot.cmake"
    helper.parent.mkdir()
    shutil.copyfile(Path(__file__).resolve().parents[2] / "cmake/HoroGnsSelectedRoot.cmake", helper)
    patch_dir = helper.parent / "patches/gns"
    patch_dir.mkdir(parents=True)
    patch = f"--- a/src/sentinel.cpp\n+++ b/src/sentinel.cpp\n@@ -1 +1 @@\n-{ORIGINAL_SOURCE}+{PATCHED_SOURCE}"
    patch_path = patch_dir / "horo-gns-selected-root-v1.patch"
    patch_path.write_bytes(patch.encode())
    digest = lambda text: hashlib.sha256(text.encode()).hexdigest()
    manifest = {"patchSha256": digest(patch), "licenseSha256": digest("fixture license\n"),
                "paths": [{"path": "src/sentinel.cpp", "pristineSha256": digest(ORIGINAL_SOURCE),
                           "candidateSha256": digest(PATCHED_SOURCE)} ]}
    (patch_dir / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    _prepare_patch_repository(project, patch_path, git, pytester, autocrlf_checkout)
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
        assert (owned / "src/sentinel.cpp").read_bytes() == PATCHED_SOURCE.encode()
        assert parent_source.read_text() == ORIGINAL_SOURCE
        assert (pristine / "src/sentinel.cpp").read_text() == ORIGINAL_SOURCE
    (owned / "src/sentinel.cpp").write_text("corrupted\n", encoding="utf-8")
    rejected = pytester.run(cmake, "-P", str(script), timeout=15)
    assert rejected.ret != 0
    assert "source changed" in rejected.stderr.str()
    assert parent_source.read_text() == ORIGINAL_SOURCE
    assert (pristine / "src/sentinel.cpp").read_text() == ORIGINAL_SOURCE
