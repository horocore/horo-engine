"""Synthetic records exercise the XR smoke format and gate."""
from __future__ import annotations

from copy import deepcopy
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
    smoke["checks"]["recovery"].update(status="Failed", issue="XR-123")
    with pytest.raises(ValueError, match="recovery did not pass"):
        xr.check_physical_gate(xr.validate_record(smoke), declared(smoke), set())


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
