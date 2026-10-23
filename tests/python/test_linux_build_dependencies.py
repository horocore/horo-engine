"""Exercise the CI installer without root access, package changes, or network I/O."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess  # nosec B404 - fixed repository installer and local command fixtures.

import pytest


ROOT = Path(__file__).resolve().parents[2]
INSTALLER = ROOT / ".github/scripts/install_linux_build_dependencies.sh"
PACKAGES = set("""
ninja-build ccache lld unzip python3-venv xvfb xauth libssl-dev
libwayland-dev wayland-protocols libxkbcommon-dev
libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
libxext-dev libxss-dev libxtst-dev libxxf86vm-dev
libgl1-mesa-dev libglu1-mesa-dev
libvulkan1 mesa-vulkan-drivers vulkan-validationlayers vulkan-tools glslang-tools
""".split())

SHIM = r'''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

name, args = Path(sys.argv[0]).name, sys.argv[1:]
scenario = os.environ["INSTALLER_SCENARIO"]
if name == "lsb_release":
    print("Ubuntu" if "--id" in args else "noble")
    sys.exit(0)
if name == "dpkg":
    print("arm64" if scenario == "unsupported" else "amd64")
    sys.exit(0)
if name == "sudo":
    assert args.pop(0) == "-n"
    os.execvp(args[0], args)

if name == "timeout" and args[3] in {"apt-cache", "dpkg-query"}:
    assert args[:2] == ["--signal=TERM", "--kill-after=2s"]
    assert args[2] == ("10s" if args[3] == "apt-cache" else "5s")
    os.execvp(args[3], args[3:])
if name == "dpkg-query":
    with open(os.environ["INSTALLER_LOG"], "a") as log:
        log.write(json.dumps({"name": name, "args": args, "phase": "installed-versions"}) + "\n")
    print("libssl-dev\t3.5.5-1ubuntu3.7")
    sys.exit(0)

options = [arg for arg in args if arg.startswith("Dir::")]
source = Path(next(arg.split("=", 1)[1] for arg in options if "sourcelist=" in arg))
sources = source.read_text()
primary = "https://archive.ubuntu.com/ubuntu" in sources
phase = "policy" if name == "apt-cache" else "update" if "update" in args else "download" if "--download-only" in args else "install"
preferences = next((arg.split("=", 1)[1] for arg in options if "::preferences=" in arg), None)
isolated = (preferences is not None and Path(preferences).read_text() == ""
            and "Dir::Etc::preferencesparts=-" in args)
if scenario == "inherited_pin" and phase == "download" and not isolated:
    print("E: Packages were downgraded and -y was used without --allow-downgrades.")
    sys.exit(100)
with open(os.environ["INSTALLER_LOG"], "a") as log:
    log.write(json.dumps({"name": name, "args": args, "sources": sources, "phase": phase,
                         "primary": primary, "isolatedPreferences": isolated, "noninteractive": os.environ.get("DEBIAN_FRONTEND")}) + "\n")
if name == "timeout":
    assert args[:2] == ["--signal=TERM", "--kill-after=10s"]
    assert args[2] == ("45s" if phase == "update" else "90s")
    assert phase != "install"
    if primary and phase == "download" and scenario in {"timeout", "cancel"}:
        sys.exit(124 if scenario == "timeout" else 143)
    os.execvp(args[3], args[3:])
if name == "apt-cache":
    print("libssl-dev: Installed: 3.5.5-1ubuntu3.7 Candidate: 3.5.5-1ubuntu3.7")
    sys.exit(0)
assert name == "apt-get"
if scenario == "unmet_dependencies" and phase == "download":
    print("E: Unmet dependencies. A required version is not available.")
    sys.exit(100)
if scenario == "both_fail" and phase == "download":
    sys.exit(100)
if scenario == "index_failure" and primary and phase == "update":
    sys.exit(100)
if scenario == "install_failure" and phase == "install":
    sys.exit(100)
if phase == "install" and scenario in {"install_cancel", "install_interrupt"}:
    sys.exit(143 if scenario == "install_cancel" else 130)
sys.exit(0)
'''


def run_installer(tmp_path: Path, scenario: str = "success") -> tuple[subprocess.CompletedProcess, list[dict]]:
    tools = tmp_path / "command fixtures"
    tools.mkdir()
    for name in ("sudo", "timeout", "apt-get", "apt-cache", "dpkg-query", "lsb_release", "dpkg"):
        path = tools / name
        path.write_text(SHIM, encoding="utf-8")
        path.chmod(0o755)
    temporary = tmp_path / "runner temp ü"
    temporary.mkdir()
    log = tmp_path / "calls.jsonl"
    environment = {**os.environ, "PATH": f"{tools}{os.pathsep}{os.environ['PATH']}",
                   "RUNNER_TEMP": str(temporary), "INSTALLER_LOG": str(log),
                   "INSTALLER_SCENARIO": scenario, "GITHUB_STEP_SUMMARY": str(tmp_path / "summary.md")}
    result = subprocess.run(["/bin/bash", str(INSTALLER)], env=environment, capture_output=True,  # nosec B603
                            text=True, check=False, shell=False, timeout=15)
    calls = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
    assert not list(temporary.iterdir()), result.stderr
    return result, calls


def test_success_preserves_dependencies_and_installs_offline_after_acquisition(tmp_path: Path) -> None:
    result, calls = run_installer(tmp_path)
    assert result.returncode == 0, result.stderr
    apt = [call for call in calls if call["name"] == "apt-get"]
    assert [call["phase"] for call in apt] == ["update", "download", "install"]
    for call in apt:
        assert "Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg" in call["sources"]
        assert "Suites: noble noble-updates noble-backports noble-security" in call["sources"]
        assert "APT::Update::Error-Mode=any" in call["args"]
        assert "Dir::Etc::sourceparts=-" in call["args"]
        assert call["isolatedPreferences"]
        assert "APT::Get::allow-downgrades=false" in call["args"]
        assert "--allow-downgrades" not in call["args"]
        assert "--allow-unauthenticated" not in call["args"]
        assert "trusted=yes" not in call["sources"]
    for call in apt[1:]:
        assert set(call["args"][-len(PACKAGES):]) == PACKAGES
    assert "--no-download" in apt[-1]["args"]
    assert apt[-1]["noninteractive"] == "noninteractive"
    assert "installation | 0" in (tmp_path / "summary.md").read_text()


@pytest.mark.parametrize("scenario", ["timeout", "index_failure"])
def test_failed_acquisition_switches_mirror_before_installing(tmp_path: Path, scenario: str) -> None:
    result, calls = run_installer(tmp_path, scenario)
    assert result.returncode == 0, result.stderr
    apt = [call for call in calls if call["name"] == "apt-get"]
    installs = [call for call in apt if call["phase"] == "install"]
    assert len(installs) == 1 and not installs[0]["primary"]
    assert "http://azure.archive.ubuntu.com/ubuntu" in installs[0]["sources"]
    assert [call["phase"] for call in apt if not call["primary"]] == ["update", "download", "install"]
    if scenario == "index_failure":
        assert [call["phase"] for call in apt if call["primary"]] == ["update"]


def test_unavailable_mirrors_fail_without_installing_any_package(tmp_path: Path) -> None:
    result, calls = run_installer(tmp_path, "both_fail")
    assert result.returncode != 0
    assert not any(call["phase"] == "install" for call in calls)
    assert "Neither Ubuntu mirror" in result.stdout


def test_install_failure_propagates_without_retrying_dpkg(tmp_path: Path) -> None:
    result, calls = run_installer(tmp_path, "install_failure")
    assert result.returncode == 100
    assert all(call.get("primary", True) for call in calls)
    assert len([call for call in calls if call["phase"] == "install"]) == 1


def test_cancellation_does_not_start_a_fallback_attempt(tmp_path: Path) -> None:
    result, calls = run_installer(tmp_path, "cancel")
    assert result.returncode == 143
    assert all(call["primary"] for call in calls)
    assert not any(call["phase"] == "install" for call in calls)


def test_unsupported_runner_fails_before_acquisition(tmp_path: Path) -> None:
    result, calls = run_installer(tmp_path, "unsupported")
    assert result.returncode != 0
    assert not calls


def test_image_pins_do_not_override_isolated_official_package_policy(tmp_path: Path) -> None:
    """Model an inherited image pin; this verifies policy, not the hosted cause."""
    result, calls = run_installer(tmp_path, "inherited_pin")
    assert result.returncode == 0, result.stderr
    apt = [call for call in calls if call["name"] == "apt-get"]
    assert [call["phase"] for call in apt] == ["update", "download", "install"]
    assert all(call["isolatedPreferences"] for call in apt)
    assert all("APT::Get::allow-downgrades=false" in call["args"] for call in apt)


def test_genuine_solver_failure_stays_failed_with_bounded_scoped_diagnostics(tmp_path: Path) -> None:
    result, calls = run_installer(tmp_path, "unmet_dependencies")
    assert result.returncode != 0
    assert "Unmet dependencies" in result.stdout
    assert not any(call["phase"] == "install" for call in calls)
    policy = [call for call in calls if call["name"] == "apt-cache"]
    installed = [call for call in calls if call["name"] == "dpkg-query"]
    assert len(policy) == len(installed) == 1
    assert policy[0]["isolatedPreferences"]
    assert "policy" in policy[0]["args"]
    assert set(policy[0]["args"][-len(PACKAGES):]) == PACKAGES
    assert set(installed[0]["args"][-len(PACKAGES):]) == PACKAGES
    assert installed[0]["args"][:2] == ["-W", r"-f=${binary:Package}\t${Version}\n"]
    assert "--allow-downgrades" not in policy[0]["args"]


@pytest.mark.parametrize("scenario, expected", [("install_cancel", 143), ("install_interrupt", 130)])
def test_installation_cancellation_propagates_without_diagnostics_or_retry(
    tmp_path: Path, scenario: str, expected: int,
) -> None:
    result, calls = run_installer(tmp_path, scenario)
    assert result.returncode == expected
    assert [call["phase"] for call in calls if call["name"] == "apt-get"] == ["update", "download", "install"]
    assert not any(call["name"] in {"apt-cache", "dpkg-query"} for call in calls)
