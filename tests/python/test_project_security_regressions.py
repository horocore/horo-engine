"""Executable trust and bounded parser regressions for portable project tooling."""
import builtins
import json
import os
from pathlib import Path
import sys
from unittest.mock import patch

import pytest

import project


@pytest.mark.parametrize("runner", ["version", "android"])
@pytest.mark.parametrize("command", ["echo injected", [], [""], ["relative/tool", "--version"], [sys.executable, "bad\0arg"]])
def test_process_runners_reject_ambiguous_argv_before_launch(tmp_path, runner, command):
    with patch.object(project.subprocess, "run") as run, patch.object(project.subprocess, "Popen") as popen:
        with pytest.raises(project.AndroidError, match="argv|executable|argument"):
            if runner == "version":
                project.version_output(command)
            else:
                project.android_run(command, tmp_path, tmp_path / "run.log")
        run.assert_not_called()
        popen.assert_not_called()


@pytest.mark.parametrize("runner", ["version", "android"])
def test_explicit_executable_preserves_literal_arguments_without_loader_injection(tmp_path, monkeypatch, runner):
    monkeypatch.setenv("PYTHONPATH", str(tmp_path / "untrusted startup"))
    monkeypatch.setenv("BASH_ENV", str(tmp_path / "untrusted shell startup"))
    monkeypatch.setenv("HORO_ANDROID_KEY_ALIAS", "selected alias")
    marker = tmp_path / "must not be created"
    literal = f"value; & echo injected > {marker} $(ignored) ü"
    code = "import json,os,sys; print(json.dumps([sys.argv[1],os.getenv('PYTHONPATH'),os.getenv('BASH_ENV'),os.getenv('HORO_ANDROID_KEY_ALIAS')]))"
    command = [sys.executable, "-c", code, literal]
    if runner == "version":
        output = project.version_output(command)
    else:
        log = tmp_path / "process.log"
        project.android_run(command, tmp_path, log, timeout=10)
        output = log.read_text(encoding="utf-8")
    assert json.loads(output) == [literal, None, None, "selected alias"]
    assert not marker.exists()


@pytest.mark.parametrize("runner", ["version", "android"])
@pytest.mark.parametrize("kind", ["unknown-name", "missing-absolute", "windows-batch", "cwd-search"])
def test_process_runners_require_explicit_executable_trust(tmp_path, monkeypatch, runner, kind):
    if kind == "unknown-name":
        command = ["unapproved-host-tool"]
    elif kind == "missing-absolute":
        command = [str(tmp_path / "missing.exe")]
    elif kind == "windows-batch":
        batch = tmp_path / "tool.cmd"
        batch.write_text("@echo injected", encoding="utf-8")
        command = [str(batch)]
    else:
        monkeypatch.chdir(tmp_path)
        monkeypatch.setenv("PATH", os.pathsep.join(["", ".", "relative-tools"]))
        monkeypatch.setenv("NoDefaultCurrentDirectoryInExePath", "1")
        (tmp_path / "cmake.exe").write_bytes(b"not a trusted tool")
        command = ["cmake", "--version"]
    with patch.object(project.subprocess, "run") as run, patch.object(project.subprocess, "Popen") as popen:
        with pytest.raises(project.AndroidError, match="executable|tool|batch"):
            if runner == "version":
                project.version_output(command)
            else:
                project.android_run(command, tmp_path, tmp_path / "run.log")
        run.assert_not_called()
        popen.assert_not_called()


def test_explicit_relative_executable_is_anchored_before_changing_child_directory(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    executable = os.path.relpath(sys.executable, tmp_path)
    child = tmp_path / "different working directory"
    child.mkdir()
    log = tmp_path / "relative.log"
    project.android_run([executable, "-c", "print('relative tool works')"], child, log, timeout=10)
    assert log.read_text().strip() == "relative tool works"


def test_android_timeout_reaps_child_on_native_host(tmp_path):
    with pytest.raises(project.AndroidError, match="timed out"):
        project.android_run([sys.executable, "-c", "import time; time.sleep(30)"],
                            tmp_path, tmp_path / "timeout.log", timeout=0.05)


def test_junit_requires_lazy_hardened_dependency_with_actionable_error(tmp_path, monkeypatch):
    report = tmp_path / "report.xml"
    report.write_text("<testsuite><testcase name='real'/></testsuite>", encoding="utf-8")
    original_import = builtins.__import__

    def without_defusedxml(name, *args, **kwargs):
        if name == "defusedxml" or name.startswith("defusedxml."):
            raise ModuleNotFoundError("defusedxml unavailable")
        return original_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", without_defusedxml)
    with pytest.raises(ValueError, match="defusedxml"):
        project.require_executed_tests(report)


@pytest.mark.parametrize("version", ["1.1١.0", "1.0.1١", "1.0.0-١", "1.0.0-01", "1.0.0-", "1.0.0-rc..1"])
def test_semver_requires_bounded_ascii_canonical_identifiers(version):
    with pytest.raises(SystemExit, match="canonical SemVer|leading-zero"):
        project.parse_version(version)


def test_semver_adversarial_input_rejected_in_deadline():
    import subprocess
    code = (
        "import sys; sys.path.insert(0, sys.argv[1]); import project; "
        "project.parse_version('1.0.0-' + 'a.' * 1000000 + '!')"
    )
    result = subprocess.run([sys.executable, "-c", code, str(Path(project.__file__).parent)],
                            capture_output=True, timeout=5, check=False)
    assert result.returncode != 0
    assert b"canonical SemVer" in result.stderr
