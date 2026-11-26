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


def test_android_system_build_digest_is_scoped_to_standalone_android(smoke):
    smoke["provenance"]["androidDeviceBuildSha256"] = DIGEST
    with pytest.raises(ValueError, match="requires a standalone Android tuple"):
        xr.validate_record(smoke)
    smoke["tuple"].update(connectionMode="Standalone", hostPlatform="Android")
    assert xr.validate_record(smoke)["provenance"]["androidDeviceBuildSha256"] == DIGEST
    smoke["provenance"]["androidDeviceBuildSha256"] = "raw-build-fingerprint"
    with pytest.raises(ValueError, match="Invalid Android system build digest"):
        xr.validate_record(smoke)


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


@pytest.mark.parametrize(("change", "error"), [
    (None, None),
    ("digest", "Display capture digest does not match"),
    ("run", "Display capture is from a different smoke run"),
    ("connection", "Display capture is from a different product tuple"),
    ("eye-order", "Display report does not match the captured runtime values"),
])
def test_device_cli_checks_runtime_display_capture(tmp_path, smoke, change, error):
    capture = {
        "schemaVersion": 1, "tuple": deepcopy(smoke["tuple"]), "provenance": deepcopy(smoke["provenance"]),
        "refreshMilliHertz": smoke["display"]["refreshMilliHertz"],
        "viewExtents": deepcopy(smoke["display"]["viewExtents"]),
    }
    if change == "run":
        capture["provenance"]["runId"] = "another-run"
    if change == "connection":
        capture["tuple"]["connectionMode"] = "WirelessStreaming"
    if change == "eye-order":
        capture["viewExtents"].reverse()
    data = json.dumps(capture).encode("utf-8")
    digest = DIGEST if change == "digest" else hashlib.sha256(data).hexdigest()
    smoke["checks"]["displayEnumeration"]["evidenceSha256"] = digest
    smoke["display"]["evidenceSha256"] = digest
    capture_path = tmp_path / "display capture.json"
    record_path = tmp_path / "smoke.json"
    expectation_path = tmp_path / "declaration.json"
    capture_path.write_bytes(data)
    record_path.write_text(json.dumps(smoke), encoding="utf-8")
    expectation_path.write_text(json.dumps(declared(smoke)), encoding="utf-8")
    result = subprocess.run(
        [sys.executable, str(SCRIPT), str(record_path), "--expect", str(expectation_path), "--display-capture", str(capture_path)],
        capture_output=True, text=True, check=False,
    )
    if error is None:
        assert result.returncode == 0, result.stderr
        assert "Runtime display capture matches the physical smoke run" in result.stdout
    else:
        assert result.returncode == 1
        assert error in result.stderr
        assert not result.stdout


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


def test_device_cli_requires_the_declared_mixed_reality_capture(tmp_path, smoke):
    record_path = tmp_path / "device.json"
    declaration_path = tmp_path / "declaration.json"
    capture_path = tmp_path / "mixed-reality.json"
    capture = {"schemaVersion": 1, "evidenceLevel": "PhysicalDeviceQualification",
               "tuple": smoke["tuple"], "provenance": smoke["provenance"],
               "capabilities": {"lightEstimation": {
                   "operation": True, "consentDenial": True, "revocation": True, "fallback": True, "shutdown": True,
               }}}
    data = json.dumps(capture).encode()
    capture_path.write_bytes(data)
    smoke["checks"]["optionalMR"]["evidenceSha256"] = hashlib.sha256(data).hexdigest()
    record_path.write_text(json.dumps(smoke), encoding="utf-8")
    declaration = dict(declared(smoke), mixedRealityCapabilities=["lightEstimation"])
    declaration_path.write_text(json.dumps(declaration), encoding="utf-8")
    command = [sys.executable, str(SCRIPT), str(record_path), "--expect", str(declaration_path), "--require", "optionalMR"]

    missing = subprocess.run(command, capture_output=True, text=True, check=False)
    assert missing.returncode != 0
    assert "must be used together" in missing.stderr
    valid = subprocess.run([*command, "--mr-capture", str(capture_path)], capture_output=True, text=True, check=False)
    assert valid.returncode == 0, valid.stderr


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


@pytest.mark.parametrize(("measurements", "error"), [
    ("2,72000,90000,2,1024,800,1032,808", None),
    ("0,4,1024,1024,896,1024,768,1024,640,1024", None),
    ("2,90000,72000,2,1024,800,1032,808", "Refresh values must be unique and sorted"),
    ("33,90000,2,1024,800,1032,808", "Invalid Desktop display rate count"),
    ("0,2,1024,800,1032", "Invalid Desktop display view count"),
    ("0,2,1024,800,1032,65537", "Invalid runtime view extent"),
    ("0,2,1024,800,1032,private", "Invalid Desktop display measurements"),
])
def test_desktop_v3_preflight_checks_measurements(desktop_identity, measurements, error):
    data = desktop_preflight_receipt(desktop_identity).replace(b"qualification-v2", b"qualification-v3").removesuffix(b"\n")
    data += f" display={measurements}\n".encode()
    digest = hashlib.sha256(data).hexdigest()
    if error is None:
        xr.check_desktop_preflight(data, desktop_identity, digest)
    else:
        with pytest.raises(ValueError, match=error):
            xr.check_desktop_preflight(data, desktop_identity, digest)


@pytest.fixture
def desktop_identity(smoke):
    identity = smoke["tuple"]
    identity.update(hostPlatform="Linux", renderer="Vulkan", runtime="Test Runtime",
                    headsetModel="Test HMD", runtimeVersion="9.8.7", connectionMode="Tethered")
    return identity


def desktop_preflight_command(tmp_path, smoke, receipt):
    record = tmp_path / "smoke.json"
    expected = tmp_path / "expected.json"
    record.write_text(json.dumps(smoke), encoding="utf-8")
    expected.write_text(json.dumps(declared(smoke)), encoding="utf-8")
    return [sys.executable, str(SCRIPT), str(record), "--expect", str(expected), "--desktop-preflight", str(receipt)]


@pytest.mark.parametrize("line_ending", [b"\n", b"\r\n"])
def test_desktop_preflight_cli_checks_exact_receipt_and_physical_gate(tmp_path, smoke, desktop_identity, line_ending):
    receipt = tmp_path / "receipt.txt"
    receipt.write_bytes(desktop_preflight_receipt(desktop_identity).removesuffix(b"\n") + line_ending)
    desktop_identity["packageReceiptSha256"] = hashlib.sha256(receipt.read_bytes()).hexdigest()
    command = desktop_preflight_command(tmp_path, smoke, receipt)
    assert subprocess.run(command, capture_output=True, check=False).returncode == 0
    smoke["evidenceLevel"] = "RuntimeCompatibility"
    (tmp_path / "smoke.json").write_text(json.dumps(smoke), encoding="utf-8")
    rejected = subprocess.run(command, capture_output=True, check=False)
    assert rejected.returncode == 1
    assert b"Physical headset evidence is required" in rejected.stderr


@pytest.mark.parametrize(("component", "corrupt_digest", "error"), [
    ("2" * 64, False, None),
    (None, False, "Separate Desktop preflight requires the package component digest"),
    ("3" * 64, False, "Desktop preflight uses a different installed component"),
    ("2" * 64, True, "Desktop preflight does not match the declared receipt digest"),
])
def test_desktop_preflight_cli_can_verify_a_separate_receipt(tmp_path, smoke, desktop_identity, component, corrupt_digest, error,
                                                               monkeypatch, capsys):
    receipt = tmp_path / "receipt.txt"
    data = desktop_preflight_receipt(desktop_identity)
    receipt.write_bytes(data)
    preflight = "f" * 64 if corrupt_digest else hashlib.sha256(data).hexdigest()
    command = desktop_preflight_command(tmp_path, smoke, receipt) + ["--desktop-preflight-sha256", preflight]
    if component is not None:
        command += ["--desktop-component-sha256", component]

    monkeypatch.setattr(sys, "argv", command[1:])
    if error is None:
        assert xr.main() == 0
        assert "Physical smoke evidence matches the declaration" in capsys.readouterr().out
    else:
        with pytest.raises(SystemExit) as rejected:
            xr.main()
        assert rejected.value.code == 1
        assert error in capsys.readouterr().err


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
