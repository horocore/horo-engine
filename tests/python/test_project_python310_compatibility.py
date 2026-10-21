"""Python 3.10 project operations must not import the author runner's TOML reader."""
import builtins
import json
from pathlib import Path
import runpy
import shutil
import sys

import pytest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/project.py"


@pytest.mark.parametrize("standalone", [False, True], ids=["source", "copied-sdk"])
@pytest.mark.parametrize("operation", ["help", "generate", "scaffold"])
def test_project_operations_without_tomllib(monkeypatch, tmp_path, capsys, standalone, operation):
    script = SCRIPT
    if standalone:
        script = tmp_path / "sdk with spaces" / "scripts/project.py"
        script.parent.mkdir(parents=True)
        shutil.copyfile(SCRIPT, script)
        assert list(script.parent.iterdir()) == [script]
        templates = script.parent.parent / "share/horo/extension-sdk/ci"
        templates.mkdir(parents=True)
        for name in ("extension-author-ci.yml", "extension-ci.lock.json.in"):
            shutil.copyfile(SCRIPT.parents[1] / "sdk/ci" / name, templates / name)
    output = tmp_path / "output"
    if operation == "help":
        args = ["--help"]
    elif operation == "generate":
        output.mkdir()
        source = tmp_path / "Behavior.cpp"
        source.write_text('HORO_BEHAVIOR(RealBehavior, "game.tests.real")\n', encoding="utf-8")
        args = ["gameplay", "--output", str(output / "bundle.cpp"),
                "--revision-output", str(output / "bundle.revision"), "--output-root", str(output),
                "--module-id", "game.tests", "--fingerprint", "test-fingerprint", str(source)]
    else:
        args = ["extension", "--shape", "hybrid", "--id", "com.example.tools",
                "--name", "Example Tools", "--output", str(output)]

    original_import = builtins.__import__

    def without_tomllib(name, *args, **kwargs):
        if name == "tomllib" or name.startswith("tomllib."):
            raise ModuleNotFoundError("No module named 'tomllib' (Python 3.10 simulation)")
        return original_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", without_tomllib)
    monkeypatch.setattr(sys, "argv", [str(script), *args])
    with pytest.raises(SystemExit) as exited:
        runpy.run_path(str(script), run_name="__main__")
    assert exited.value.code == 0
    if operation == "help":
        assert "compatibility" in capsys.readouterr().out
    elif operation == "generate":
        assert "HoroGeneratedBehaviorDescriptor_RealBehavior" in (output / "bundle.cpp").read_text()
        assert int((output / "bundle.revision").read_text()) > 0
    else:
        manifest = json.loads((output / "extension.json.in").read_text(encoding="utf-8"))
        assert manifest["id"] == "com.example.tools"
        assert len(manifest["modules"]) == 3
        assert len(list((output / "src").glob("*.c"))) == 3
