"""Verifier regressions use synthetic bytes/output; they are not native artifact qualification."""
from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys

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


def inputs(tmp_path, profile="Shipping"):
    commands, variables = artifact.expected_inventory(profile)
    binary = tmp_path / "fixture.exe"
    archive = tmp_path / "catalog.lib"
    binary.write_bytes("\0".join(sorted(commands | variables)).encode("ascii"))
    core_controls = {artifact.HISTORY, "console.visible", "overlay.logs.visible", "overlay.jobs.visible"}
    archive.write_bytes("\0".join(sorted((commands - (artifact.CORE | {"sys.metrics"})) | (variables - core_controls))).encode("ascii"))
    return binary, archive


def fixture_evidence(profile, binary, archive):
    output = binary.with_suffix(".manifest")
    output.write_text(manifest(profile), encoding="utf-8")
    return (output, hashlib.sha256(binary.read_bytes()).hexdigest(),
            hashlib.sha256(archive.read_bytes()).hexdigest())


def qualify(profile, binary, archive):
    return artifact.qualify(profile, binary, archive, *fixture_evidence(profile, binary, archive), "0")


@pytest.mark.parametrize("profile", artifact.PROFILES)
def test_complete_synthetic_observation_records_identity_hashes_and_execution(tmp_path, profile):
    binary, archive = inputs(tmp_path, profile)
    report = qualify(profile, binary, archive)
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
def test_unavailable_metadata_is_rejected_in_either_artifact(tmp_path, kind, profile, denied):
    binary, archive = inputs(tmp_path, profile)
    path = binary if kind == "binary" else archive
    path.write_bytes(path.read_bytes() + b"\0" + denied.encode("ascii"))
    with pytest.raises(artifact.QualificationError, match="denied metadata"):
        qualify(profile, binary, archive)


@pytest.mark.parametrize("kind", ["binary", "archive"])
def test_missing_positive_controls_cannot_pass_an_empty_or_unrelated_image(tmp_path, kind):
    binary, archive = inputs(tmp_path)
    (binary if kind == "binary" else archive).write_bytes(b"unrelated image")
    with pytest.raises(artifact.QualificationError, match="positive metadata controls"):
        qualify("Shipping", binary, archive)


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


def test_artifact_replacement_during_execution_invalidates_observation(tmp_path):
    binary, archive = inputs(tmp_path)
    evidence = fixture_evidence("Shipping", binary, archive)
    binary.write_bytes(binary.read_bytes() + b"changed")
    with pytest.raises(artifact.QualificationError, match="changed between"):
        artifact.qualify("Shipping", binary, archive, *evidence, "0")


def test_nonzero_exit_does_not_accept_an_apparently_complete_manifest(tmp_path):
    binary, archive = inputs(tmp_path)
    with pytest.raises(artifact.QualificationError, match="did not exit successfully"):
        artifact.qualify("Shipping", binary, archive, *fixture_evidence("Shipping", binary, archive), "1")


def test_manifest_file_read_is_bounded(tmp_path, monkeypatch):
    binary, archive = inputs(tmp_path)
    evidence = fixture_evidence("Shipping", binary, archive)
    monkeypatch.setattr(artifact, "MAX_MANIFEST_BYTES", 16)
    with pytest.raises(artifact.QualificationError, match="output budget"):
        artifact.qualify("Shipping", binary, archive, *evidence, "0")


def test_binary_scan_catches_names_split_across_chunk_boundaries(tmp_path):
    binary = tmp_path / "split.exe"
    binary.write_bytes(b"x" * (1024 * 1024 - 5) + b"console.history.limit")
    assert artifact.scan_artifact(binary, {artifact.HISTORY})["present"] == [artifact.HISTORY]


@pytest.mark.parametrize("profile", ["Editor", "Development"])
def test_shared_scene_metadata_is_required_in_linked_binary_but_not_defaults_archive(tmp_path, profile):
    binary, archive = inputs(tmp_path, profile)
    for name in ("game.scene_tree", "game.inspect", "overlay.scene.visible"):
        archive.write_bytes(archive.read_bytes().replace(name.encode("ascii"), b""))
    assert qualify(profile, binary, archive)["status"] == "passed"
    binary.write_bytes(binary.read_bytes().replace(b"game.scene_tree", b""))
    with pytest.raises(artifact.QualificationError, match="positive metadata controls"):
        qualify(profile, binary, archive)


@pytest.mark.parametrize("profile", artifact.PROFILES)
@pytest.mark.parametrize("kind", ["binary", "archive"])
def test_optional_application_inspection_is_not_part_of_the_catalog_core_fixture(tmp_path, profile, kind):
    binary, archive = inputs(tmp_path, profile)
    path = binary if kind == "binary" else archive
    path.write_bytes(path.read_bytes() + b"\0net.debug_status")
    with pytest.raises(artifact.QualificationError, match="denied metadata"):
        qualify(profile, binary, archive)


@pytest.mark.parametrize("profile", ["Editor", "Development", "Profile", "Diagnostics"])
def test_shared_network_panel_metadata_remains_required_in_the_final_image(tmp_path, profile):
    binary, archive = inputs(tmp_path, profile)
    archive.write_bytes(archive.read_bytes().replace(b"overlay.network.visible", b""))
    assert qualify(profile, binary, archive)["status"] == "passed"
    binary.write_bytes(binary.read_bytes().replace(b"overlay.network.visible", b""))
    with pytest.raises(artifact.QualificationError, match="positive metadata controls"):
        qualify(profile, binary, archive)


@pytest.mark.parametrize("failure,code,status", [("timeout", 2, "incomplete"), ("missing", 2, "incomplete"), ("leak", 1, "failed")])
def test_cli_never_records_a_pass_for_missing_timed_out_or_denied_artifacts(tmp_path, monkeypatch, failure, code, status):
    binary, archive = inputs(tmp_path)
    output, binary_hash, archive_hash = fixture_evidence("Shipping", binary, archive)
    report = tmp_path / "report.json"
    if failure == "missing":
        binary.unlink()
    elif failure == "leak":
        archive.write_bytes(archive.read_bytes() + b"\0console.history.limit")
    result = "unavailable" if failure == "timeout" else "0"
    monkeypatch.setattr(sys, "argv", ["qualify_console_artifact.py", "--profile", "Shipping", "--binary", str(binary),
                                     "--archive", str(archive), "--manifest", str(output), "--binary-sha256", binary_hash,
                                     "--archive-sha256", archive_hash, "--execution-result", result, "--report", str(report)])
    assert artifact.main() == code
    assert json.loads(report.read_text(encoding="utf-8"))["status"] == status
