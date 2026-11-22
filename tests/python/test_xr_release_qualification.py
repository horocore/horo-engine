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
COMPONENT = "1" * 64


def store(root, content):
    digest = hashlib.sha256(content).hexdigest()
    (root / digest).write_bytes(content)
    return digest


def make_claim(root, mode, category="Reference"):
    identity = dict.fromkeys(IDENTITY_FIELDS, "synthetic")
    identity.update(buildSha256=store(root, f"{mode} executable".encode()),
                    packageReceiptSha256=store(root, f"{mode} package receipt".encode()),
                    connectionMode=mode, admittedExtensions=["XR_EXT_fixture"])
    if mode != "Standalone":
        identity.update(hostPlatform="Linux", renderer="Vulkan", runtimeVersion="1.2.3")
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
    row = {
        "id": mode, "category": category, "owner": "xr-platform", "releaseClaim": True,
        "declaration": {"tuple": identity, "testPlan": plan}, "requiredOptionalChecks": ["haptics"],
        "recordSha256": store(root, json.dumps(record).encode()),
    }
    if mode != "Standalone":
        row["desktopPreflightSha256"] = store(root, desktop_receipt(row))
        row["desktopComponentSha256"] = COMPONENT
    return row


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
    planned.update(category="Future", releaseClaim=False, declaration=None, recordSha256=None, desktopPreflightSha256=None)
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


def update_claim_identity(row, evidence, **changes):
    identity = row["declaration"]["tuple"]
    identity.update(changes)
    record = json.loads((evidence / row["recordSha256"]).read_bytes())
    record["tuple"] = identity
    row["recordSha256"] = store(evidence, json.dumps(record).encode())


def desktop_receipt(row, *, host=None):
    identity = row["declaration"]["tuple"]
    system = f"{identity['runtime']}\0{identity['headsetModel']}".encode()
    target = hashlib.sha256(b"Horo Desktop OpenXR probe v1\0" + system).hexdigest()
    host = host or identity["hostPlatform"].lower()
    return (f"horo-xr-desktop-qualification-v1 sha256:{COMPONENT} sha256:{identity['buildSha256']} "
            f"{host} system-default vulkan sha256:{target} desktop-local preflight 1 2 3 1 3 0 2 2 3\n").encode()


@pytest.mark.parametrize(("host", "mode"), [("Windows", "Tethered"), ("Linux", "WirelessStreaming")])
def test_desktop_claim_requires_and_consumes_packaged_preflight(release, host, mode):
    matrix, evidence = release
    row = matrix["tuples"][1]
    update_claim_identity(row, evidence, hostPlatform=host, renderer="Vulkan", runtimeVersion="1.2.3", connectionMode=mode)
    row["desktopPreflightSha256"] = None
    missing = run_matrix(release)
    assert missing.returncode == 1
    assert "Tethered (xr-platform): Required Desktop preflight was not run" in missing.stderr

    row["desktopPreflightSha256"] = store(evidence, desktop_receipt(row))
    passed = run_matrix(release)
    assert passed.returncode == 0, passed.stderr


def test_legacy_desktop_claim_uses_the_package_receipt_when_it_is_the_preflight(release):
    matrix, evidence = release
    row = matrix["tuples"][1]
    del row["desktopPreflightSha256"]
    del row["desktopComponentSha256"]
    failed = run_matrix(release)
    assert failed.returncode == 1
    assert "Invalid Desktop preflight" in failed.stderr

    receipt = store(evidence, desktop_receipt(row))
    update_claim_identity(row, evidence, packageReceiptSha256=receipt)
    passed = run_matrix(release)
    assert passed.returncode == 0, passed.stderr


@pytest.mark.parametrize(("component", "reason"), [
    (None, "Separate Desktop preflight requires the package component digest"),
    ("2" * 64, "Desktop preflight uses a different installed component"),
])
def test_separate_preflight_must_match_the_declared_package_component(release, component, reason):
    matrix, _ = release
    matrix["tuples"][1]["desktopComponentSha256"] = component
    result = run_matrix(release)
    assert result.returncode == 1
    assert f"Tethered (xr-platform): {reason}" in result.stderr


def test_every_claim_requires_its_own_runtime_evidence(release):
    matrix, evidence = release
    windows = matrix["tuples"][1]
    linux = make_claim(evidence, "WirelessStreaming", "Compatibility")
    matrix["tuples"].append(linux)
    for row, host in ((windows, "Windows"), (linux, "Linux")):
        update_claim_identity(row, evidence, hostPlatform=host, renderer="Vulkan", runtimeVersion="1.2.3")
    windows["desktopPreflightSha256"] = store(evidence, desktop_receipt(windows))
    linux["desktopPreflightSha256"] = store(evidence, desktop_receipt(linux, host="windows"))
    result = run_matrix(release)
    assert result.returncode == 1
    assert "Tethered (xr-platform): Passed" in result.stdout
    assert "WirelessStreaming (xr-platform): Desktop preflight uses a different host" in result.stderr


def test_digest_matching_desktop_receipt_cannot_cover_a_standalone_claim(release):
    matrix, evidence = release
    desktop = matrix["tuples"][1]
    update_claim_identity(desktop, evidence, hostPlatform="Windows", renderer="Vulkan", runtimeVersion="1.2.3")
    receipt = store(evidence, desktop_receipt(desktop))
    desktop["desktopPreflightSha256"] = receipt
    matrix["tuples"][0]["desktopPreflightSha256"] = receipt
    result = run_matrix(release)
    assert result.returncode == 1
    assert "Standalone (xr-platform): Desktop preflight is unavailable for this tuple" in result.stderr


@pytest.mark.parametrize(("host", "renderer"), [("macOS", "Vulkan"), ("Windows", "D3D12"), ("Windows 11", "Vulkan")])
def test_unsupported_pcvr_route_cannot_pass_without_desktop_evidence(release, host, renderer):
    matrix, evidence = release
    row = matrix["tuples"][1]
    update_claim_identity(row, evidence, hostPlatform=host, renderer=renderer)
    del row["desktopPreflightSha256"]
    result = run_matrix(release)
    assert result.returncode == 1
    assert "Tethered (xr-platform): Unsupported PCVR host or renderer" in result.stderr
