"""Synthetic matrix artifacts exercise aggregation and gate behavior."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys

import pytest


SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPTS))
try:
    from qualify_xr_device import ALL_CHECKS, IDENTITY_FIELDS
    from qualify_xr_release import REVIEWS
finally:
    sys.path.pop(0)

REVISION = "a" * 40


def store(root, content):
    digest = hashlib.sha256(content).hexdigest()
    (root / digest).write_bytes(content)
    return digest


def make_claim(root, mode, category="Reference"):
    identity = dict.fromkeys(IDENTITY_FIELDS, "synthetic")
    identity.update(buildSha256=store(root, f"{mode} executable".encode()),
                    packageReceiptSha256=store(root, f"{mode} package receipt".encode()),
                    connectionMode=mode, admittedExtensions=["XR_EXT_fixture"])
    observation = store(root, f"{mode} observation".encode())
    plan = "xr-smoke-v1"
    record = {
        "schemaVersion": 1, "category": category, "evidenceLevel": "PhysicalDeviceQualification",
        "status": "Passed", "tuple": identity, "provenance": {"testPlan": plan, "runId": mode},
        "checks": {name: {"status": "Passed", "evidenceSha256": observation, "issue": None}
                   for name in ALL_CHECKS},
        "display": {"evidenceSha256": observation, "refreshMilliHertz": [90000],
                    "viewExtents": [{"width": 1024, "height": 800}, {"width": 1032, "height": 808}]},
    }
    return {
        "id": mode, "category": category, "owner": "xr-platform", "releaseClaim": True,
        "declaration": {"tuple": identity, "testPlan": plan}, "requiredOptionalChecks": ["haptics"],
        "recordSha256": store(root, json.dumps(record).encode()),
    }


@pytest.fixture
def release(tmp_path):
    evidence = tmp_path / "XR evidence"
    evidence.mkdir()
    matrix = {
        "schemaVersion": 1, "sourceRevision": REVISION,
        "reviews": {name: {"owner": f"xr-{name}", "status": "Passed", "sourceRevision": REVISION,
                           "evidenceLevel": "DeterministicFake", "artifactSha256": store(evidence, name.encode())}
                    for name in REVIEWS},
        "tuples": [make_claim(evidence, "Standalone"), make_claim(evidence, "Tethered")],
    }
    return matrix, evidence


def run_matrix(release):
    matrix, evidence = release
    plan = evidence.parent / "release-matrix.json"
    plan.write_text(json.dumps(matrix), encoding="utf-8")
    return subprocess.run(
        [sys.executable, str(SCRIPTS / "qualify_xr_release.py"), str(plan), "--evidence-dir", str(evidence)],
        capture_output=True, text=True, check=False,
    )


def test_reference_pair_and_optional_planning_owner(release):
    matrix, evidence = release
    planned = make_claim(evidence, "WirelessStreaming", "Compatibility")
    planned.update(category="Future", releaseClaim=False, declaration=None, recordSha256=None)
    matrix["tuples"].append(planned)
    result = run_matrix(release)
    assert result.returncode == 0, result.stderr
    assert "WirelessStreaming (xr-platform): Future planning" in result.stdout
    assert "XR 1.0 matrix evidence passed" in result.stdout


def test_a_nonreference_pcvr_claim_cannot_fill_the_reference_requirement(release):
    matrix, evidence = release
    matrix["tuples"][1] = make_claim(evidence, "WirelessStreaming", "Compatibility")
    result = run_matrix(release)
    assert result.returncode == 1
    assert "Missing declared PCVR reference tuple" in result.stderr


def test_no_extra_declared_device_can_disappear_from_the_gate(release):
    matrix, evidence = release
    claimed = make_claim(evidence, "WirelessStreaming", "Compatibility")
    claimed["recordSha256"] = None
    matrix["tuples"].append(claimed)
    result = run_matrix(release)
    assert result.returncode == 1
    assert "WirelessStreaming (xr-platform): Missing or invalid artifact digest" in result.stderr


def test_stale_review_and_corrupt_receipt_are_both_reported(release):
    matrix, evidence = release
    matrix["reviews"]["renderer"]["sourceRevision"] = "b" * 40
    receipt = matrix["tuples"][0]["declaration"]["tuple"]["packageReceiptSha256"]
    (evidence / receipt).write_bytes(b"modified")
    result = run_matrix(release)
    assert result.returncode == 1
    assert "renderer (xr-renderer): Subsystem review is from another source revision" in result.stderr
    assert "Standalone (xr-platform): Artifact content does not match" in result.stderr


def test_hashed_simulator_smoke_record_never_fulfills_physical_claim(release):
    matrix, evidence = release
    row = matrix["tuples"][0]
    record = json.loads((evidence / row["recordSha256"]).read_bytes())
    record["evidenceLevel"] = "SimulatorReplay"
    row["recordSha256"] = store(evidence, json.dumps(record).encode())
    result = run_matrix(release)
    assert result.returncode == 1
    assert "Standalone (xr-platform): Physical headset evidence is required" in result.stderr
