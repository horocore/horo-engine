"""Verifier regressions use synthetic bytes/output; they are not native artifact qualification."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("console_artifact", ROOT / "scripts/qualify_console_artifact.py")
assert SPEC and SPEC.loader
artifact = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(artifact)


def manifest(profile):
    commands, variables = artifact.expected_inventory(profile)
    lines = ["horo-console-artifact\t1", f"profile\t{profile}"]
    lines += [f"command\t{name}" for name in sorted(commands)]
    lines += [f"variable\t{name}" for name in sorted(variables)]
    lines += [f"passed\t{name}" for name in sorted(artifact.CORE)]
    lines += [f"{'omitted' if profile == 'Shipping' else 'passed'}\thistory"]
    return "\n".join(lines) + "\n"


def inputs(tmp_path, monkeypatch, profile="Shipping"):
    commands, variables = artifact.expected_inventory(profile)
    binary = tmp_path / "fixture.exe"
    archive = tmp_path / "catalog.lib"
    binary.write_bytes("\0".join(sorted(commands | variables)).encode("ascii"))
    core_controls = {artifact.HISTORY, "console.visible", "overlay.logs.visible", "overlay.jobs.visible"}
    archive.write_bytes("\0".join(sorted((commands - (artifact.CORE | {"sys.metrics"})) | (variables - core_controls))).encode("ascii"))
    monkeypatch.setattr(artifact.subprocess, "run", lambda *args, **kwargs:
                        SimpleNamespace(returncode=0, stdout=manifest(profile)))
    return binary, archive


@pytest.mark.parametrize("profile", artifact.PROFILES)
def test_complete_synthetic_observation_records_identity_hashes_and_execution(tmp_path, monkeypatch, profile):
    binary, archive = inputs(tmp_path, monkeypatch, profile)
    report = artifact.qualify(profile, binary, archive)
    assert report["status"] == "passed"
    assert report["profile"] == profile
    assert report["scope"] == "catalog-and-core-fixture"
    assert len(report["artifacts"]["binary"]["sha256"]) == 64
    assert report["artifacts"]["binary"]["path"] == str(binary.resolve())
    assert report["execution"]["history"] == ("omitted" if profile == "Shipping" else "passed")


@pytest.mark.parametrize("kind", ["binary", "archive"])
@pytest.mark.parametrize("profile,denied", [("Shipping", "console.history.limit"), ("Profile", "game.inspect"),
                                           ("Profile", "diag.support_bundle"), ("Diagnostics", "game.scene_tree"),
                                           ("Server", "screenshot")])
def test_unavailable_metadata_is_rejected_in_either_artifact(tmp_path, monkeypatch, kind, profile, denied):
    binary, archive = inputs(tmp_path, monkeypatch, profile)
    path = binary if kind == "binary" else archive
    path.write_bytes(path.read_bytes() + b"\0" + denied.encode("ascii"))
    with pytest.raises(artifact.QualificationError, match="denied metadata"):
        artifact.qualify(profile, binary, archive)


@pytest.mark.parametrize("kind", ["binary", "archive"])
def test_missing_positive_controls_cannot_pass_an_empty_or_unrelated_image(tmp_path, monkeypatch, kind):
    binary, archive = inputs(tmp_path, monkeypatch)
    (binary if kind == "binary" else archive).write_bytes(b"unrelated image")
    with pytest.raises(artifact.QualificationError, match="positive metadata controls"):
        artifact.qualify("Shipping", binary, archive)


@pytest.mark.parametrize("change", ["profile", "missing", "duplicate", "execution", "unknown", "header"])
def test_incomplete_or_contradictory_native_output_is_rejected(change):
    output = manifest("Shipping")
    if change == "profile":
        output = manifest("Development")
    elif change == "missing":
        output = output.replace("command\tscreenshot\n", "")
    elif change == "duplicate":
        output += "command\tversion\n"
    elif change == "execution":
        output = output.replace("omitted\thistory", "passed\thistory")
    elif change == "unknown":
        output += "arbitrary\tdata\n"
    else:
        output = output.replace("horo-console-artifact\t1", "horo-console-artifact\t2")
    with pytest.raises(artifact.QualificationError):
        artifact.verify_manifest("Shipping", output)


def test_artifact_replacement_during_execution_invalidates_observation(tmp_path, monkeypatch):
    binary, archive = inputs(tmp_path, monkeypatch)
    def replace(*args, **kwargs):
        binary.write_bytes(binary.read_bytes() + b"changed")
        return SimpleNamespace(returncode=0, stdout=manifest("Shipping"))
    monkeypatch.setattr(artifact.subprocess, "run", replace)
    with pytest.raises(artifact.QualificationError, match="changed between"):
        artifact.qualify("Shipping", binary, archive)


def test_nonzero_exit_does_not_accept_an_apparently_complete_manifest(tmp_path, monkeypatch):
    binary, archive = inputs(tmp_path, monkeypatch)
    monkeypatch.setattr(artifact.subprocess, "run", lambda *args, **kwargs:
                        SimpleNamespace(returncode=1, stdout=manifest("Shipping")))
    with pytest.raises(artifact.QualificationError, match="exited with 1"):
        artifact.qualify("Shipping", binary, archive)


def test_binary_scan_catches_names_split_across_chunk_boundaries(tmp_path):
    binary = tmp_path / "split.exe"
    binary.write_bytes(b"x" * (1024 * 1024 - 5) + b"console.history.limit")
    assert artifact.scan_artifact(binary, {artifact.HISTORY})["present"] == [artifact.HISTORY]


@pytest.mark.parametrize("profile", ["Editor", "Development"])
def test_shared_scene_metadata_is_required_in_linked_binary_but_not_defaults_archive(tmp_path, monkeypatch, profile):
    binary, archive = inputs(tmp_path, monkeypatch, profile)
    for name in ("game.scene_tree", "game.inspect", "overlay.scene.visible"):
        archive.write_bytes(archive.read_bytes().replace(name.encode("ascii"), b""))
    assert artifact.qualify(profile, binary, archive)["status"] == "passed"
    binary.write_bytes(binary.read_bytes().replace(b"game.scene_tree", b""))
    with pytest.raises(artifact.QualificationError, match="positive metadata controls"):
        artifact.qualify(profile, binary, archive)


@pytest.mark.parametrize("profile", artifact.PROFILES)
@pytest.mark.parametrize("kind", ["binary", "archive"])
def test_optional_application_inspection_is_not_part_of_the_catalog_core_fixture(tmp_path, monkeypatch, profile, kind):
    binary, archive = inputs(tmp_path, monkeypatch, profile)
    path = binary if kind == "binary" else archive
    path.write_bytes(path.read_bytes() + b"\0net.debug_status")
    with pytest.raises(artifact.QualificationError, match="denied metadata"):
        artifact.qualify(profile, binary, archive)


@pytest.mark.parametrize("profile", ["Editor", "Development", "Profile", "Diagnostics"])
def test_shared_network_panel_metadata_remains_required_in_the_final_image(tmp_path, monkeypatch, profile):
    binary, archive = inputs(tmp_path, monkeypatch, profile)
    archive.write_bytes(archive.read_bytes().replace(b"overlay.network.visible", b""))
    assert artifact.qualify(profile, binary, archive)["status"] == "passed"
    binary.write_bytes(binary.read_bytes().replace(b"overlay.network.visible", b""))
    with pytest.raises(artifact.QualificationError, match="positive metadata controls"):
        artifact.qualify(profile, binary, archive)


def test_timeout_remains_an_incomplete_observation(tmp_path, monkeypatch):
    binary, archive = inputs(tmp_path, monkeypatch)
    def timeout(*args, **kwargs):
        raise subprocess.TimeoutExpired(args[0], 30)
    monkeypatch.setattr(artifact.subprocess, "run", timeout)
    with pytest.raises(subprocess.TimeoutExpired):
        artifact.qualify("Shipping", binary, archive)


@pytest.mark.parametrize("failure,code,status", [("timeout", 2, "incomplete"), ("missing", 2, "incomplete"), ("leak", 1, "failed")])
def test_cli_never_records_a_pass_for_missing_timed_out_or_denied_artifacts(tmp_path, monkeypatch, failure, code, status):
    binary, archive = inputs(tmp_path, monkeypatch)
    report = tmp_path / "report.json"
    if failure == "missing":
        binary.unlink()
    elif failure == "leak":
        archive.write_bytes(archive.read_bytes() + b"\0console.history.limit")
    else:
        def timeout(*args, **kwargs):
            raise subprocess.TimeoutExpired(args[0], 30)
        monkeypatch.setattr(artifact.subprocess, "run", timeout)
    monkeypatch.setattr(sys, "argv", ["qualify_console_artifact.py", "--profile", "Shipping", "--binary", str(binary),
                                     "--archive", str(archive), "--report", str(report)])
    assert artifact.main() == code
    assert json.loads(report.read_text(encoding="utf-8"))["status"] == status
