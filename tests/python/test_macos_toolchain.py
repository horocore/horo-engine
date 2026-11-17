"""Native Mac admission and optional hosted Xcode component provisioning."""
from pathlib import Path
import os
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / ".github/scripts/prepare_macos_toolchain.sh"


@pytest.fixture
def tools(tmp_path, monkeypatch):
    if os.name != "posix":
        pytest.skip("Apple command fixtures require a POSIX shell")
    log = tmp_path / "calls"
    state = tmp_path / "installed"
    monkeypatch.setenv("HORO_TEST_TOOL_LOG", str(log))
    monkeypatch.setenv("HORO_TEST_METAL_STATE", str(state))
    monkeypatch.setenv("RUNNER_OS", "macOS")
    monkeypatch.setenv("RUNNER_ENVIRONMENT", "github-hosted")
    monkeypatch.delenv("HORO_TEST_DOWNLOAD_FAIL", raising=False)
    monkeypatch.delenv("HORO_TEST_STILL_MISSING", raising=False)
    monkeypatch.setenv("PATH", str(tmp_path) + os.pathsep + os.environ["PATH"])
    scripts = {
        "xcrun": 'printf "xcrun %s\\n" "$*" >> "$HORO_TEST_TOOL_LOG"\n'
                 'test -f "$HORO_TEST_METAL_STATE"\n',
        "xcodebuild": 'printf "xcodebuild %s\\n" "$*" >> "$HORO_TEST_TOOL_LOG"\n'
                      'if [ "${HORO_TEST_DOWNLOAD_FAIL:-}" = 1 ]; then exit 17; fi\n'
                      'if [ "${HORO_TEST_STILL_MISSING:-}" != 1 ]; then touch "$HORO_TEST_METAL_STATE"; fi\n',
    }
    for name, body in scripts.items():
        path = tmp_path / name
        path.write_text("#!/bin/sh\nset -eu\n" + body)
        path.chmod(0o755)
    return log, state


def run():
    return subprocess.run(["/bin/bash", str(SCRIPT)], capture_output=True, text=True, timeout=5, check=False)


def test_preinstalled_compiler_never_downloads(tools):
    log, state = tools
    state.touch()
    assert run().returncode == 0
    assert log.read_text().splitlines() == ["xcrun --sdk macosx metal --version"]


def test_hosted_missing_component_is_installed_and_executed_again(tools):
    log, _ = tools
    assert run().returncode == 0
    assert log.read_text().splitlines() == [
        "xcrun --sdk macosx metal --version",
        "xcodebuild -downloadComponent MetalToolchain",
        "xcrun --sdk macosx metal --version",
    ]


@pytest.mark.parametrize("environment", ["self-hosted", ""])
def test_custom_or_unknown_host_is_not_modified(tools, monkeypatch, environment):
    log, _ = tools
    monkeypatch.setenv("RUNNER_ENVIRONMENT", environment)
    assert run().returncode != 0
    assert log.read_text().splitlines() == ["xcrun --sdk macosx metal --version"]


@pytest.mark.parametrize("failure", ["HORO_TEST_DOWNLOAD_FAIL", "HORO_TEST_STILL_MISSING"])
def test_download_or_execution_failure_cannot_report_ready(tools, monkeypatch, failure):
    monkeypatch.setenv(failure, "1")
    assert run().returncode != 0


def test_non_native_runner_executes_no_apple_tools(tools, monkeypatch):
    log, _ = tools
    monkeypatch.setenv("RUNNER_OS", "Linux")
    assert run().returncode != 0
    assert not log.exists()


def test_hardware_runner_requires_successful_producer_and_trusted_event():
    workflow = (ROOT / ".github/workflows/ci.yml").read_text()
    route = next(line for line in workflow.splitlines() if "vars.HORO_MACOS_METAL_RUNNER" in line)
    assert "needs.macos-build.result == 'success' || needs.macos-build-main.result == 'success'" in route
    assert "vars.HORO_MACOS_METAL_RUNNER != ''" in route
    assert "github.event_name == 'push'" in route
    assert "github.event_name == 'workflow_dispatch'" in route
    assert "github.event_name == 'pull_request' && github.event.pull_request.head.repo.full_name == github.repository" in route
    assert "|| 'macos-latest'" in route
    assert "|| 'ubuntu-slim'" in route


def test_native_producer_verifies_metal_before_temporary_directory():
    workflow = (ROOT / ".github/workflows/ci.yml").read_text()
    native = workflow.split("\n  test:\n", 1)[1].split("\n  macos-build:\n", 1)[0]
    admission = native.split("- name: Canonicalize macOS temporary directory", 1)[1].split("- name:", 1)[0]
    assert "if: matrix.name == 'macOS / Clang'" in admission
    assert 'if [[ "$RUNNER_OS" != macOS ]]' in admission
    assert admission.index("xcrun --sdk macosx metal --version") < admission.index("tempfile.gettempdir()")
