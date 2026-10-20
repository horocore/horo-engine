"""Provisioned parser dependencies and executable invocation identity contracts."""
from pathlib import Path
import importlib.metadata
import os
import re
import sys

import pytest

import analyze
import project


ROOT = Path(__file__).resolve().parents[2]


def test_hardened_xml_dependency_is_pinned_for_repository_and_standalone_sdk():
    pin = "defusedxml==" + importlib.metadata.version("defusedxml")
    requirements = (ROOT / "scripts/requirements.txt").read_text(encoding="utf-8")
    workflow = (ROOT / "sdk/ci/extension-author-ci.yml").read_text(encoding="utf-8")
    documentation = (ROOT / "sdk/ExtensionSdkREADME.md.in").read_text(encoding="utf-8")
    assert pin in requirements.splitlines()
    assert re.search(r"python -m pip install[^\n]*\b" + re.escape(pin) + r"(?:\s|$)", workflow)
    assert pin in documentation
    assert "no extra Python package dependency" not in documentation


@pytest.mark.parametrize("tool", ["analyze", "project"])
@pytest.mark.parametrize("selection", ["absolute", "PATH"])
def test_executable_admission_preserves_invocation_path(tmp_path, monkeypatch, tool, selection):
    # Windows need not grant symlink privilege: model only dereferencing here.
    invocation = tmp_path / "venv" / Path(sys.executable).name
    invocation.parent.mkdir()
    invocation.write_bytes(b"executable fixture")
    invocation.chmod(0o700)
    target = tmp_path / "base" / invocation.name
    original_resolve = Path.resolve

    def resolve(path, *args, **kwargs):
        return target if path == invocation else original_resolve(path, *args, **kwargs)

    monkeypatch.setattr(Path, "resolve", resolve)
    monkeypatch.setenv("PATH", str(invocation.parent))
    argv = [str(invocation) if selection == "absolute" else invocation.name, "literal ; argument"]
    admitted = (analyze._command_executable(argv, {"PATH": str(invocation.parent)})
                if tool == "analyze" else project.admitted_command(argv)[0])
    assert admitted == str(invocation)


@pytest.mark.parametrize("selection", ["absolute", "PATH"])
def test_missing_analysis_executable_never_launches(tmp_path, monkeypatch, selection):
    import threading
    from unittest.mock import patch

    missing = tmp_path / "vanished.exe"
    monkeypatch.setattr(analyze.shutil, "which", lambda *args, **kwargs: str(missing))
    lifecycle = {}
    with patch.object(analyze.subprocess, "Popen") as launch:
        with pytest.raises(analyze.AnalysisError, match="executable"):
            analyze.native_run([str(missing) if selection == "absolute" else missing.name],
                               tmp_path, 1, {"PATH": str(tmp_path)}, threading.Event(), lifecycle)
        launch.assert_not_called()
    assert lifecycle["outcome"] == "no_escape"


@pytest.mark.parametrize("selection", ["absolute", "PATH"])
def test_missing_project_executable_never_launches(tmp_path, monkeypatch, selection):
    from unittest.mock import patch

    missing = tmp_path / "vanished.exe"
    monkeypatch.setenv("PATH", str(tmp_path))
    monkeypatch.setattr(project.shutil, "which", lambda *args, **kwargs: str(missing))
    argv = [str(missing) if selection == "absolute" else missing.name]
    with patch.object(project.subprocess, "run") as run, patch.object(project.subprocess, "Popen") as launch:
        with pytest.raises(project.AndroidError, match="executable"):
            project.version_output(argv)
        with pytest.raises(project.AndroidError, match="executable"):
            project.android_run(argv, tmp_path, tmp_path / "child.log")
        run.assert_not_called()
        launch.assert_not_called()


def test_execution_environment_keeps_trusted_jvm_options(monkeypatch):
    options = {"JAVA_TOOL_OPTIONS": "-javaagent:trusted-agent.jar", "_JAVA_OPTIONS": "-Xmx512m",
               "JDK_JAVA_OPTIONS": "-Dcaller=trusted", "GRADLE_OPTS": "-Dorg.gradle.daemon=false"}
    for key, value in options.items():
        monkeypatch.setenv(key, value)
    assert options.items() <= project.execution_environment().items()


def test_copied_sdk_junit_validation_uses_installed_dependency(tmp_path):
    import shutil
    import subprocess

    shutil.copyfile(ROOT / "scripts/project.py", tmp_path / "project.py")
    report = tmp_path / "ctest.xml"
    report.write_text('<testsuite><testcase name="executed"/></testsuite>', encoding="utf-8")
    code = (
        "import pathlib,sys; sys.path.insert(0,sys.argv[1]); import project,defusedxml; "
        "assert pathlib.Path(project.__file__).parent == pathlib.Path(sys.argv[1]); "
        "project.require_executed_tests(pathlib.Path(sys.argv[2])); "
        "print(defusedxml.__file__)"
    )
    result = subprocess.run([sys.executable, "-I", "-c", code, str(tmp_path), str(report)],
                            cwd=tmp_path, capture_output=True, text=True, timeout=30, check=False)
    assert result.returncode == 0, result.stderr
    assert "site-packages" in result.stdout


@pytest.mark.skipif(os.name == "nt", reason="POSIX venv executable symlinks")
@pytest.mark.parametrize("tool", ["analyze", "project-version", "project-android"])
@pytest.mark.parametrize("selection", ["absolute", "PATH"])
def test_posix_venv_child_retains_prefix_and_dependency_path(tmp_path, monkeypatch, tool, selection):
    import json
    import threading
    import venv

    environment = tmp_path / "venv"
    venv.EnvBuilder(with_pip=False, symlinks=True).create(environment)
    executable = environment / "bin/python"
    assert executable.is_symlink()
    sites = next((environment / "lib").glob("python*/site-packages"))
    dependency = sites / "invocation_dependency.py"
    dependency.write_text("VALUE = 'venv-only'\n", encoding="utf-8")
    monkeypatch.setenv("PATH", str(executable.parent))
    code = "import json,sys,invocation_dependency as d; print(json.dumps([sys.prefix,d.__file__,d.VALUE]))"
    argv = [str(executable) if selection == "absolute" else "python", "-c", code]
    if tool == "analyze":
        output = analyze.native_run(argv, tmp_path, 10, dict(os.environ), threading.Event()).stdout
    elif tool == "project-version":
        output = project.version_output(argv)
    else:
        log = tmp_path / "child.log"
        project.android_run(argv, tmp_path, log, timeout=10)
        output = log.read_text(encoding="utf-8")
    prefix, path, value = json.loads(output)
    assert Path(prefix) == environment
    assert Path(path) == dependency
    assert value == "venv-only"
