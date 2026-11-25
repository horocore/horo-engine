"""Synthetic records exercise the XR smoke format and gate."""
from __future__ import annotations

from copy import deepcopy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

import pytest


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "qualify_xr_device.py"
SPEC = importlib.util.spec_from_file_location("qualify_xr_device", SCRIPT)
assert SPEC and SPEC.loader
xr = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(xr)
DIGEST = "1" * 64


@pytest.fixture
def smoke():
    identity = dict.fromkeys(xr.IDENTITY_FIELDS, "synthetic")
    identity.update(buildSha256=DIGEST, packageReceiptSha256=DIGEST,
                    connectionMode="Tethered", admittedExtensions=["XR_EXT_fixture"])
    return {
        "schemaVersion": 1, "category": "Reference", "evidenceLevel": "PhysicalDeviceQualification",
        "status": "Passed", "tuple": identity,
        "provenance": {"testPlan": "xr-smoke-v1", "runId": "synthetic-fixture"},
        "checks": {name: {"status": "Passed", "evidenceSha256": DIGEST, "issue": None}
                   for name in xr.ALL_CHECKS},
        "display": {"evidenceSha256": DIGEST, "refreshMilliHertz": [72000, 90000],
                    "viewExtents": [{"width": 1024, "height": 800}, {"width": 1032, "height": 808}]},
    }


def declared(smoke):
    return {"tuple": deepcopy(smoke["tuple"]), "testPlan": smoke["provenance"]["testPlan"]}


@pytest.mark.parametrize("mode", sorted(xr.CONNECTION_MODES))
def test_each_connection_matches_only_its_own_complete_tuple(smoke, mode):
    smoke["tuple"]["connectionMode"] = mode
    expected = declared(smoke)
    xr.check_physical_gate(xr.validate_record(smoke), expected, set())
    for other in xr.CONNECTION_MODES - {mode}:
        expected["tuple"]["connectionMode"] = other
        with pytest.raises(ValueError, match="declared product tuple"):
            xr.check_physical_gate(smoke, expected, set())
    expected = declared(smoke)
    expected["tuple"]["driverVersion"] = "other-driver"
    with pytest.raises(ValueError, match="declared product tuple"):
        xr.check_physical_gate(smoke, expected, set())


@pytest.mark.parametrize("level", sorted(xr.LEVELS - {"PhysicalDeviceQualification"}))
def test_nonphysical_levels_cannot_pass(smoke, level):
    smoke["evidenceLevel"] = level
    with pytest.raises(ValueError, match="Physical headset evidence"):
        xr.check_physical_gate(xr.validate_record(smoke), declared(smoke), set())


@pytest.mark.parametrize("status", ["NotRun", "Blocked"])
def test_unavailable_hardware_cannot_pass(smoke, status):
    smoke["status"] = status
    for check in smoke["checks"].values():
        check.update(status=status, evidenceSha256=None, issue="missing-test-device")
    smoke["display"] = None
    with pytest.raises(ValueError, match="did not pass"):
        xr.check_physical_gate(xr.validate_record(smoke), declared(smoke), set())


def test_display_requires_runtime_enumeration_and_a_matching_capture(smoke):
    smoke["display"]["refreshMilliHertz"] = []
    with pytest.raises(ValueError, match="enumerated refresh"):
        xr.validate_record(smoke)
    smoke["display"]["refreshMilliHertz"] = [90000]
    smoke["display"]["evidenceSha256"] = "2" * 64
    with pytest.raises(ValueError, match="display check's runtime capture"):
        xr.validate_record(smoke)


def test_optional_limitation_is_visible_and_required_claims_are_enforced(smoke):
    smoke["status"] = "PassedWithLimitations"
    smoke["checks"]["haptics"].update(status="NotRun", evidenceSha256=None, issue="not-in-release-plan")
    xr.check_physical_gate(xr.validate_record(smoke), declared(smoke), set())
    with pytest.raises(ValueError, match="haptics did not pass"):
        xr.check_physical_gate(smoke, declared(smoke), {"haptics"})
    smoke["checks"]["recovery"].update(status="PassedWithLimitations", issue="XR-123")
    with pytest.raises(ValueError, match="recovery did not pass"):
        xr.check_physical_gate(xr.validate_record(smoke), declared(smoke), set())


def test_a_limited_record_requires_a_passing_observation(smoke):
    smoke["status"] = "PassedWithLimitations"
    for check in smoke["checks"].values():
        check.update(status="Blocked", evidenceSha256=None, issue="missing-test-device")
    smoke["display"] = None
    with pytest.raises(ValueError, match="requires a passing observation"):
        xr.validate_record(smoke)


def test_cli_rejects_duplicate_record_members(tmp_path, smoke):
    record = tmp_path / "smoke.json"
    expected = tmp_path / "release.json"
    record.write_text(json.dumps(smoke), encoding="utf-8")
    expected.write_text(json.dumps(declared(smoke)), encoding="utf-8")
    argv = [sys.executable, str(SCRIPT), str(record), "--expect", str(expected)]
    assert subprocess.run(argv, capture_output=True, check=False).returncode == 0
    record.write_text(record.read_text(encoding="utf-8").replace(
        '"schemaVersion": 1', '"schemaVersion": 1, "schemaVersion": 1'), encoding="utf-8")
    failed = subprocess.run(argv, capture_output=True, check=False)
    assert failed.returncode == 1
    assert b"Duplicate JSON field" in failed.stderr


def desktop_preflight_receipt(identity):
    labels = f"{identity['runtime']}\0{identity['headsetModel']}".encode()
    target = hashlib.sha256(b"Horo Desktop OpenXR probe v1\0" + labels).hexdigest()
    return (f"horo-xr-desktop-qualification-v2 sha256:{'2' * 64} sha256:{identity['buildSha256']} "
            f"linux system-default vulkan sha256:{target} tethered preflight 9 8 7 1 3 0 2 2 3\n").encode()


@pytest.fixture
def desktop_identity(smoke):
    identity = smoke["tuple"]
    identity.update(hostPlatform="Linux", renderer="Vulkan", runtime="Test Runtime",
                    headsetModel="Test HMD", runtimeVersion="9.8.7", connectionMode="Tethered")
    return identity


@pytest.mark.parametrize("line_ending", [b"\n", b"\r\n"])
def test_desktop_preflight_cli_checks_exact_receipt_and_physical_gate(tmp_path, smoke, desktop_identity, line_ending):
    receipt = tmp_path / "receipt.txt"
    receipt.write_bytes(desktop_preflight_receipt(desktop_identity).removesuffix(b"\n") + line_ending)
    desktop_identity["packageReceiptSha256"] = hashlib.sha256(receipt.read_bytes()).hexdigest()
    record = tmp_path / "smoke.json"
    expected = tmp_path / "expected.json"
    record.write_text(json.dumps(smoke), encoding="utf-8")
    expected.write_text(json.dumps(declared(smoke)), encoding="utf-8")
    command = [sys.executable, str(SCRIPT), str(record), "--expect", str(expected), "--desktop-preflight", str(receipt)]
    assert subprocess.run(command, capture_output=True, check=False).returncode == 0
    smoke["evidenceLevel"] = "RuntimeCompatibility"
    record.write_text(json.dumps(smoke), encoding="utf-8")
    rejected = subprocess.run(command, capture_output=True, check=False)
    assert rejected.returncode == 1
    assert b"Physical headset evidence is required" in rejected.stderr


@pytest.mark.parametrize(("field", "different", "message"), [
    ("buildSha256", "a" * 64, "different executable"),
    ("runtime", "Other Runtime", "different runtime or system"),
    ("headsetModel", "Other HMD", "different runtime or system"),
    ("runtimeVersion", "9.8.6", "different runtime version"),
    ("hostPlatform", "Windows", "different host"),
    ("renderer", "Metal", "different runtime route or renderer"),
    ("connectionMode", "Standalone", "PCVR declaration"),
    ("connectionMode", "WirelessStreaming", "different connection mode"),
    ("packageReceiptSha256", "b" * 64, "declared receipt digest"),
])
def test_desktop_preflight_rejects_foreign_tuple(desktop_identity, field, different, message):
    receipt = desktop_preflight_receipt(desktop_identity)
    desktop_identity["packageReceiptSha256"] = hashlib.sha256(receipt).hexdigest()
    desktop_identity[field] = different
    with pytest.raises(ValueError, match=message):
        xr.check_desktop_preflight(receipt, desktop_identity, desktop_identity["packageReceiptSha256"])


@pytest.mark.parametrize(("index", "value", "message"), [
    (0, b"horo-xr-desktop-qualification-v1", "Expected a successful Desktop preflight"),
    (7, b"desktop-local", "different connection mode"),
    (-1, b"2", "required provider route"),
    (-1, b"16", "supported range"),
    (-9, b"0", "required runtime or renderer"),
    (-2, b"0", "required runtime or renderer"),
    (-5, b"-1", "Invalid Desktop preflight fact"),
    (-10, b"permission-denied", "Expected a successful Desktop preflight"),
])
def test_desktop_preflight_rejects_incompatible_receipt(desktop_identity, index, value, message):
    fields = desktop_preflight_receipt(desktop_identity).rstrip().split(b" ")
    fields[index] = value
    receipt = b" ".join(fields) + b"\r\n"
    desktop_identity["packageReceiptSha256"] = hashlib.sha256(receipt).hexdigest()
    with pytest.raises(ValueError, match=message):
        xr.check_desktop_preflight(receipt, desktop_identity, desktop_identity["packageReceiptSha256"])
