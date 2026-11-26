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
ANDROID_BUILD = "c" * 64


def store(root, content):
    digest = hashlib.sha256(content).hexdigest()
    (root / digest).write_bytes(content)
    return digest


def display_capture(record):
    return {"schemaVersion": 1, "tuple": record["tuple"], "provenance": record["provenance"],
            "refreshMilliHertz": record["display"]["refreshMilliHertz"], "viewExtents": record["display"]["viewExtents"]}


def store_display_capture(root, record, content=None):
    if content is None:
        content = json.dumps(display_capture(record)).encode()
    digest = store(root, content)
    record["display"]["evidenceSha256"] = digest
    record["checks"]["displayEnumeration"]["evidenceSha256"] = digest


def timing_capture(record):
    return {"schemaVersion": 1, "tuple": record["tuple"], "provenance": record["provenance"],
            "targetRefreshMilliHertz": 90000, "durationNs": 1_000_000_000, "frameCount": 90,
            "peakFrameIntervalNs": 12_000_000, "missedDeadlines": 0, "swapchainFailures": 0}


def store_timing_capture(root, record):
    record["checks"]["renderTiming"]["evidenceSha256"] = store(root, json.dumps(timing_capture(record)).encode())


def store_physical_check_captures(root, record):
    for name in ("setup", "input", "comfort", "haptics", "lifecycle", "recovery"):
        artifact = store(root, f"redacted {record['provenance']['runId']} {name} observation".encode())
        capture = {"schemaVersion": 1, "evidenceLevel": "PhysicalDeviceQualification", "tuple": record["tuple"],
                   "provenance": record["provenance"], "check": name, "artifactSha256": artifact}
        record["checks"][name]["evidenceSha256"] = store(root, json.dumps(capture).encode())


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
        "status": "Passed", "tuple": identity,
        "provenance": {"testPlan": plan, "runId": mode, "sourceRevision": REVISION},
        "checks": {name: {"status": "Passed", "evidenceSha256": observation, "issue": None}
                   for name in ALL_CHECKS},
        "display": {"evidenceSha256": observation, "refreshMilliHertz": [90000],
                    "viewExtents": [{"width": 1024, "height": 800}, {"width": 1032, "height": 808}]},
    }
    store_display_capture(root, record)
    store_timing_capture(root, record)
    store_physical_check_captures(root, record)
    row = {
        "id": mode, "category": category, "owner": "xr-platform", "releaseClaim": True,
        "declaration": {"tuple": identity, "testPlan": plan}, "requiredOptionalChecks": ["haptics"],
        "recordSha256": store(root, json.dumps(record).encode()),
        "renderTimingLimits": {"minimumDurationNs": 1_000_000_000, "minimumFrameCount": 90,
                               "maximumPeakFrameIntervalNs": 12_000_000, "maximumMissedDeadlines": 0,
                               "maximumSwapchainFailures": 0},
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


@pytest.mark.parametrize("check", ["input", "comfort"])
def test_physical_observation_cannot_be_rewrapped_for_another_connection(release, check):
    matrix, evidence = release
    first_row, second_row = matrix["tuples"]
    first = json.loads((evidence / first_row["recordSha256"]).read_bytes())
    second = json.loads((evidence / second_row["recordSha256"]).read_bytes())
    original = json.loads((evidence / first["checks"]["input"]["evidenceSha256"]).read_bytes())
    replacement = json.loads((evidence / second["checks"][check]["evidenceSha256"]).read_bytes())
    replacement["artifactSha256"] = original["artifactSha256"]
    second["checks"][check]["evidenceSha256"] = store(evidence, json.dumps(replacement).encode())
    second_row["recordSha256"] = store(evidence, json.dumps(second).encode())

    result = run_matrix(release)
    assert result.returncode == 1
    assert "Tethered (xr-platform): Physical observation artifact was reused across smoke runs" in result.stderr


def test_one_physical_observation_can_support_multiple_checks_in_the_same_run(release):
    matrix, evidence = release
    row = matrix["tuples"][0]
    record = json.loads((evidence / row["recordSha256"]).read_bytes())
    first = json.loads((evidence / record["checks"]["input"]["evidenceSha256"]).read_bytes())
    second = json.loads((evidence / record["checks"]["comfort"]["evidenceSha256"]).read_bytes())
    second["artifactSha256"] = first["artifactSha256"]
    record["checks"]["comfort"]["evidenceSha256"] = store(evidence, json.dumps(second).encode())
    row["recordSha256"] = store(evidence, json.dumps(record).encode())

    result = run_matrix(release)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("view_count", [1, 4])
def test_xr_1_0_claim_requires_physical_stereo_capture(release, view_count):
    matrix, evidence = release
    row = matrix["tuples"][1]
    record = json.loads((evidence / row["recordSha256"]).read_bytes())
    record["display"]["viewExtents"] = [{"width": 1024, "height": 800}] * view_count
    store_display_capture(evidence, record)
    row["recordSha256"] = store(evidence, json.dumps(record).encode())
    result = run_matrix(release)
    assert result.returncode == 1
    assert "XR 1.0 requires a primary stereo display capture" in result.stderr


@pytest.mark.parametrize(("measurements", "error"), [
    ("0,2,1024,800,1032,808", None),
    ("0,4,1024,800,1032,808,640,480,648,488", "Desktop preflight must report primary stereo for XR 1.0"),
])
def test_xr_1_0_claim_requires_stereo_in_a_measured_desktop_probe(release, measurements, error):
    matrix, evidence = release
    row = matrix["tuples"][1]
    preflight = desktop_receipt(row).replace(b"qualification-v2", b"qualification-v3").removesuffix(b"\n")
    row["desktopPreflightSha256"] = store(evidence, preflight + f" display={measurements}\n".encode())
    result = run_matrix(release)
    if error is None:
        assert result.returncode == 0, result.stderr
    else:
        assert result.returncode == 1
        assert error in result.stderr


@pytest.mark.parametrize(("changes", "reason"), [
    ({"durationNs": 999_999_999}, "did not meet the required duration"),
    ({"frameCount": 89}, "did not meet the required duration"),
    ({"peakFrameIntervalNs": 12_000_001}, "exceeded the declared limits"),
    ({"missedDeadlines": 1}, "exceeded the declared limits"),
    ({"swapchainFailures": 1}, "exceeded the declared limits"),
    ({"targetRefreshMilliHertz": 120000}, "target was not enumerated"),
    ({"frameCount": True}, "Invalid render timing measurements"),
    ({"provenance": {"runId": "foreign"}}, "from a different smoke run"),
    ({"tuple": {"connectionMode": "WirelessStreaming"}}, "from a different product tuple"),
    ({"privateData": "raw-frame-details"}, "Unexpected fields in render timing capture"),
])
def test_release_requires_timing_evidence_from_the_declared_run(release, changes, reason):
    matrix, evidence = release
    row = matrix["tuples"][1]
    record = json.loads((evidence / row["recordSha256"]).read_bytes())
    capture = timing_capture(record) | changes
    record["checks"]["renderTiming"]["evidenceSha256"] = store(evidence, json.dumps(capture).encode())
    row["recordSha256"] = store(evidence, json.dumps(record).encode())
    result = run_matrix(release)
    assert result.returncode == 1
    assert reason in result.stderr
    assert "raw-frame-details" not in result.stderr


def test_release_claim_requires_explicit_timing_limits(release):
    matrix, _ = release
    del matrix["tuples"][0]["renderTimingLimits"]
    result = run_matrix(release)
    assert result.returncode == 1
    assert "Claimed tuple requires render timing limits" in result.stderr


def test_release_rejects_a_generic_artifact_with_a_valid_digest_for_timing(release):
    matrix, evidence = release
    row = matrix["tuples"][1]
    record = json.loads((evidence / row["recordSha256"]).read_bytes())
    record["checks"]["renderTiming"]["evidenceSha256"] = store(evidence, b"{}")
    row["recordSha256"] = store(evidence, json.dumps(record).encode())
    result = run_matrix(release)
    assert result.returncode == 1
    assert "Unexpected fields in render timing capture" in result.stderr


def test_a_failed_unclaimed_capability_cannot_be_reported_as_limited(release):
    matrix, evidence = release
    row = matrix["tuples"][0]
    record = json.loads((evidence / row["recordSha256"]).read_bytes())
    record["status"] = "PassedWithLimitations"
    record["checks"]["optionalMR"].update(status="Failed", issue="XR-123")
    row["recordSha256"] = store(evidence, json.dumps(record).encode())
    result = run_matrix(release)
    assert result.returncode == 1
    assert "Standalone (xr-platform): A failed check requires a Failed record" in result.stderr


@pytest.fixture
def mr_claim(release):
    matrix, evidence = release
    row = matrix["tuples"][0]
    record = json.loads((evidence / row["recordSha256"]).read_bytes())
    row["requiredOptionalChecks"].append("optionalMR")
    row["declaration"]["mixedRealityCapabilities"] = ["passthrough", "spatialAnchorPersistence"]
    extensions = ["XR_FB_passthrough", "XR_MSFT_spatial_anchor", "XR_MSFT_spatial_anchor_persistence"]
    row["declaration"]["tuple"]["admittedExtensions"] = extensions.copy()
    record["tuple"]["admittedExtensions"] = extensions.copy()
    store_display_capture(evidence, record)
    store_timing_capture(evidence, record)
    store_physical_check_captures(evidence, record)
    row["recordSha256"] = store(evidence, json.dumps(record).encode())
    checks = dict.fromkeys(("operation", "consentDenial", "revocation", "fallback", "shutdown"), True)
    capture = {"schemaVersion": 1, "evidenceLevel": "PhysicalDeviceQualification",
               "tuple": record["tuple"].copy(), "provenance": record["provenance"].copy(),
               "capabilities": {"passthrough": checks.copy(),
                                "spatialAnchorPersistence": dict(checks, restore=True, staleLocalization=True)}}
    return row, record, capture


def test_release_rejects_a_mixed_reality_claim_without_its_required_extensions(release, mr_claim):
    _, evidence = release
    row, record, capture = mr_claim
    for identity in (row["declaration"]["tuple"], record["tuple"]):
        identity["admittedExtensions"].remove("XR_MSFT_spatial_anchor")
    store_display_capture(evidence, record)
    store_timing_capture(evidence, record)
    record["checks"]["optionalMR"]["evidenceSha256"] = store(evidence, json.dumps(capture).encode())
    row["recordSha256"] = store(evidence, json.dumps(record).encode())
    result = run_matrix(release)
    assert result.returncode == 1
    assert "spatialAnchorPersistence capability is missing admitted OpenXR extensions XR_MSFT_spatial_anchor" in result.stderr


def test_claimed_mixed_reality_requires_named_capabilities_and_typed_evidence(release, mr_claim):
    row, _, _ = mr_claim
    result = run_matrix(release)
    assert result.returncode == 1
    assert "Standalone (xr-platform): Invalid mixed reality capture JSON" in result.stderr
    del row["declaration"]["mixedRealityCapabilities"]
    result = run_matrix(release)
    assert result.returncode == 1
    assert "Unexpected fields in release declaration" in result.stderr


@pytest.mark.parametrize(("variation", "error"), [
    ("valid", None),
    ("simulator", "Mixed reality capture requires physical headset evidence"),
    ("tuple", "Mixed reality capture is from a different product tuple"),
    ("run", "Mixed reality capture is from a different smoke run"),
    ("omitted", "Unexpected fields in mixed reality capabilities"),
    ("revocation", "Required passthrough scenario did not pass"),
    ("persistence", "Required spatialAnchorPersistence scenario did not pass"),
    ("private", "Unexpected fields in passthrough evidence"),
    ("digest", "Mixed reality capture digest does not match"),
])
def test_release_binds_mixed_reality_capabilities_to_physical_run(release, mr_claim, variation, error):
    _, evidence = release
    row, record, capture = mr_claim
    if variation == "simulator":
        capture["evidenceLevel"] = "SimulatorReplay"
    elif variation == "tuple":
        capture["tuple"]["connectionMode"] = "Tethered"
    elif variation == "run":
        capture["provenance"]["runId"] = "different-run"
    elif variation == "omitted":
        del capture["capabilities"]["spatialAnchorPersistence"]
    elif variation == "revocation":
        capture["capabilities"]["passthrough"]["revocation"] = False
    elif variation == "persistence":
        capture["capabilities"]["spatialAnchorPersistence"]["staleLocalization"] = 1
    elif variation == "private":
        capture["capabilities"]["passthrough"]["cameraPayload"] = "private-camera-payload"
    digest = store(evidence, json.dumps(capture).encode())
    if variation == "digest":
        (evidence / digest).write_bytes(b"different bytes")
    record["checks"]["optionalMR"]["evidenceSha256"] = digest
    row["recordSha256"] = store(evidence, json.dumps(record).encode())

    result = run_matrix(release)
    if error is None:
        assert result.returncode == 0, result.stderr
    else:
        assert result.returncode == 1
        assert f"Standalone (xr-platform): {error}" in result.stderr
        assert "private-camera-payload" not in result.stderr


def test_unrun_mixed_reality_blocks_an_explicit_claim(release, mr_claim):
    _, evidence = release
    row, record, _ = mr_claim
    record["status"] = "PassedWithLimitations"
    record["checks"]["optionalMR"].update(status="NotRun", evidenceSha256=None, issue="missing-test-device")
    row["recordSha256"] = store(evidence, json.dumps(record).encode())

    result = run_matrix(release)
    assert result.returncode == 1
    assert "Standalone (xr-platform): Required physical check optionalMR did not pass" in result.stderr


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


def test_another_connection_cannot_supply_the_required_input_capture(release):
    matrix, evidence = release
    standalone, pcvr = matrix["tuples"]
    first = json.loads((evidence / standalone["recordSha256"]).read_bytes())
    second = json.loads((evidence / pcvr["recordSha256"]).read_bytes())
    second["checks"]["input"]["evidenceSha256"] = first["checks"]["input"]["evidenceSha256"]
    pcvr["recordSha256"] = store(evidence, json.dumps(second).encode())
    result = run_matrix(release)
    assert result.returncode == 1
    assert "Tethered (xr-platform): input capture is from a different product tuple" in result.stderr


@pytest.mark.parametrize(("change", "reason"), [
    ({"check": "recovery"}, "input capture is for a different check"),
    ({"provenance": {"runId": "old-run"}}, "input capture is from a different smoke run"),
    ({"evidenceLevel": "DeterministicFake"}, "input capture must identify physical evidence"),
    ({"artifactSha256": "b" * 64}, "Missing regular artifact"),
    ({"artifactSha256": []}, "Missing or invalid artifact digest"),
])
def test_required_capture_identifies_its_physical_check_and_artifact(release, change, reason):
    matrix, evidence = release
    row = matrix["tuples"][0]
    record = json.loads((evidence / row["recordSha256"]).read_bytes())
    check = record["checks"]["input"]
    capture = json.loads((evidence / check["evidenceSha256"]).read_bytes())
    capture.update(change)
    check["evidenceSha256"] = store(evidence, json.dumps(capture).encode())
    row["recordSha256"] = store(evidence, json.dumps(record).encode())
    result = run_matrix(release)
    assert result.returncode == 1
    assert f"Standalone (xr-platform): {reason}" in result.stderr


def test_a_bare_log_cannot_supply_the_required_input_capture(release):
    matrix, evidence = release
    row = matrix["tuples"][0]
    record = json.loads((evidence / row["recordSha256"]).read_bytes())
    record["checks"]["input"]["evidenceSha256"] = store(evidence, b"{}")
    row["recordSha256"] = store(evidence, json.dumps(record).encode())
    result = run_matrix(release)
    assert result.returncode == 1
    assert "Standalone (xr-platform): Unexpected fields in input capture" in result.stderr


def test_hashed_simulator_smoke_record_never_fulfills_physical_claim(release):
    matrix, evidence = release
    row = matrix["tuples"][0]
    record = json.loads((evidence / row["recordSha256"]).read_bytes())
    record["evidenceLevel"] = "SimulatorReplay"
    row["recordSha256"] = store(evidence, json.dumps(record).encode())
    result = run_matrix(release)
    assert result.returncode == 1
    assert "Standalone (xr-platform): Physical headset evidence is required" in result.stderr


@pytest.mark.parametrize(("revision", "reason"), [
    (None, "Device evidence does not record a source revision"),
    ("b" * 40, "Device evidence is from another source revision"),
    ("main", "Invalid device evidence source revision"),
])
def test_pcvr_smoke_record_must_identify_the_release_source(release, revision, reason):
    matrix, evidence = release
    row = matrix["tuples"][1]
    record = json.loads((evidence / row["recordSha256"]).read_bytes())
    del record["provenance"]["sourceRevision"]
    if revision is not None:
        record["provenance"]["sourceRevision"] = revision
    row["recordSha256"] = store(evidence, json.dumps(record).encode())

    result = run_matrix(release)
    assert result.returncode == 1
    assert f"Tethered (xr-platform): {reason}" in result.stderr


def update_claim_identity(row, evidence, **changes):
    identity = row["declaration"]["tuple"]
    identity.update(changes)
    record = json.loads((evidence / row["recordSha256"]).read_bytes())
    record["tuple"] = identity
    store_display_capture(evidence, record)
    store_timing_capture(evidence, record)
    store_physical_check_captures(evidence, record)
    row["recordSha256"] = store(evidence, json.dumps(record).encode())


def capture_android_smoke(row, evidence, *, runtime_version="1.2.3", build=ANDROID_BUILD):
    update_claim_identity(row, evidence, hostPlatform="Android", runtimeVersion=runtime_version)
    if build is not None:
        record = json.loads((evidence / row["recordSha256"]).read_bytes())
        record["provenance"]["androidDeviceBuildSha256"] = build
        store_display_capture(evidence, record)
        store_timing_capture(evidence, record)
        store_physical_check_captures(evidence, record)
        row["recordSha256"] = store(evidence, json.dumps(record).encode())


@pytest.mark.parametrize(("field", "value", "reason"), [
    ("refreshMilliHertz", [120000], "Display report does not match the captured runtime values"),
    ("viewExtents", [{"width": 1032, "height": 808}, {"width": 1024, "height": 800}],
     "Display report does not match the captured runtime values"),
    ("provenance", {"testPlan": "xr-smoke-v1", "runId": "previous-run", "sourceRevision": REVISION},
     "Display capture is from a different smoke run"),
    ("rawRoomGeometry", "private fixture data", "Unexpected fields in display capture"),
])
def test_display_capture_must_match_runtime_values_and_smoke_run(release, field, value, reason):
    matrix, evidence = release
    row = matrix["tuples"][0]
    record = json.loads((evidence / row["recordSha256"]).read_bytes())
    capture = display_capture(record)
    capture[field] = value
    store_display_capture(evidence, record, json.dumps(capture).encode())
    row["recordSha256"] = store(evidence, json.dumps(record).encode())
    result = run_matrix(release)
    assert result.returncode == 1
    assert f"Standalone (xr-platform): {reason}" in result.stderr
    assert "private fixture data" not in result.stderr


def test_display_capture_cannot_be_reused_for_a_different_connection(release):
    matrix, evidence = release
    row = matrix["tuples"][0]
    record = json.loads((evidence / row["recordSha256"]).read_bytes())
    capture = display_capture(record)
    capture["tuple"] = {**capture["tuple"], "connectionMode": "Tethered"}
    store_display_capture(evidence, record, json.dumps(capture).encode())
    row["recordSha256"] = store(evidence, json.dumps(record).encode())
    result = run_matrix(release)
    assert result.returncode == 1
    assert "Standalone (xr-platform): Display capture is from a different product tuple" in result.stderr


def android_receipt(row, evidence):
    profile = "qualification-openxr-quest-development"
    expected = {"apkSha256": store(evidence, b"separate Android diagnostic APK"),
                "deviceBuildSha256": ANDROID_BUILD, "profile": profile, "adbConnection": "usb"}
    row["androidPreflight"] = expected
    return {
        "schema": 1, "status": "diagnostic_complete", "stage": "diagnostics",
        "artifact": {"apkSha256": expected["apkSha256"], "sourceRevision": REVISION, "profile": profile,
                     "provenanceSha256": "d" * 64,
                     "openxr": {"schema": 1, "runtime": "openxr", "loaderSource": "bundled", "activityRoute": "cold-discovery"},
                     "openxrImages": {"abi": "arm64-v8a", "diagnosticHostSha256": "e" * 64, "loaderSha256": "f" * 64}},
        "qualification": {"host": {"requested": "linux-x64", "observed": "linux-x64"},
                          "connection": {"requested": "usb", "observed": "usb"}, "profile": profile,
                          "runtime": {"requested": "openxr", "selection": "system-default", "status": "cold_system_discovered"},
                          "physicalXrQualification": "not_run"},
        "device": {"abi": "arm64-v8a", "buildSha256": expected["deviceBuildSha256"]},
        "observed": {"crashObserved": False, "openxr": {"abi": "arm64-v8a", "stage": "discovered", "result": 0,
                                                       "runtimeVersion": {"major": 1, "minor": 2, "patch": 3}}},
        "cleanup": {"status": "stopped"},
    }


@pytest.mark.parametrize("pin_version", [False, True])
def test_android_discovery_can_accompany_physical_evidence_with_its_own_apk(release, pin_version):
    matrix, evidence = release
    row = matrix["tuples"][0]
    capture_android_smoke(row, evidence)
    receipt = android_receipt(row, evidence)
    if pin_version:
        receipt["qualification"]["runtime"]["expectedVersion"] = {"major": 1, "minor": 2, "patch": 3}
    row["androidPreflight"]["receiptSha256"] = store(evidence, json.dumps(receipt).encode())
    result = run_matrix(release)
    assert result.returncode == 0, result.stderr
    assert "Standalone (xr-platform): Passed" in result.stdout

    row["recordSha256"] = None
    result = run_matrix(release)
    assert result.returncode == 1
    assert "Standalone (xr-platform): Missing or invalid artifact digest" in result.stderr


@pytest.mark.parametrize(("change", "reason"), [
    (None, None),
    ("apk", "Artifact content does not match"),
    ("build", "Android preflight and physical smoke use different system builds"),
    ("revision", "Android preflight is from another source revision"),
    ("runtime", "Android preflight uses a different runtime version"),
    ("transport", "Android preflight uses a different ADB connection"),
])
def test_single_device_checker_verifies_android_deployment(release, change, reason):
    matrix, evidence = release
    row = matrix["tuples"][0]
    capture_android_smoke(row, evidence, build="9" * 64 if change == "build" else ANDROID_BUILD)
    receipt = android_receipt(row, evidence)
    if change == "revision":
        receipt["artifact"]["sourceRevision"] = "8" * 40
    if change == "runtime":
        receipt["observed"]["openxr"]["runtimeVersion"]["patch"] = 4
    if change == "transport":
        receipt["qualification"]["connection"]["observed"] = "tcpip"

    expected = row["androidPreflight"]
    expected["receiptSha256"] = store(evidence, json.dumps(receipt).encode())
    declaration = evidence.parent / "physical declaration.json"
    declaration.write_text(json.dumps(row["declaration"]), encoding="utf-8")
    preflight = evidence.parent / "Android preflight declaration.json"
    preflight.write_text(json.dumps(expected), encoding="utf-8")
    apk = evidence.parent / "diagnostic app.apk"
    apk.write_bytes(b"foreign diagnostic APK" if change == "apk" else (evidence / expected["apkSha256"]).read_bytes())

    result = subprocess.run(
        [sys.executable, str(SCRIPTS / "qualify_xr_device.py"), str(evidence / row["recordSha256"]),
         "--expect", str(declaration), "--android-preflight", str(evidence / expected["receiptSha256"]),
         "--android-preflight-declaration", str(preflight), "--android-apk", str(apk)],
        capture_output=True, text=True, check=False,
    )
    if reason is None:
        assert result.returncode == 0, result.stderr
        assert "Android OpenXR preflight matches the physical smoke run" in result.stdout
    else:
        assert result.returncode == 1
        assert reason in result.stderr
        assert not result.stdout


@pytest.mark.parametrize(("build", "reason"), [
    (None, "Physical smoke record does not identify its Android system build"),
    ("9" * 64, "Android preflight and physical smoke use different system builds"),
])
def test_android_preflight_requires_the_system_build_observed_in_physical_smoke(release, build, reason):
    matrix, evidence = release
    row = matrix["tuples"][0]
    capture_android_smoke(row, evidence, build=build)
    receipt = android_receipt(row, evidence)
    row["androidPreflight"]["receiptSha256"] = store(evidence, json.dumps(receipt).encode())

    result = run_matrix(release)
    assert result.returncode == 1
    assert f"Standalone (xr-platform): {reason}" in result.stderr


def test_android_smoke_without_preflight_can_use_the_original_provenance(release):
    matrix, evidence = release
    capture_android_smoke(matrix["tuples"][0], evidence, build=None)

    result = run_matrix(release)
    assert result.returncode == 0, result.stderr
    assert "Standalone (xr-platform): Passed" in result.stdout


def test_android_discovery_accepts_the_redacted_fields_emitted_by_deployment(release):
    matrix, evidence = release
    row = matrix["tuples"][0]
    capture_android_smoke(row, evidence)
    receipt = android_receipt(row, evidence)
    receipt["artifact"].update(applicationId="org.horocore.packagequalification.openxr.quest",
                               declaredRenderer="none", abis=["arm64-v8a"],
                               expectedApkSha256=receipt["artifact"]["apkSha256"])
    receipt["device"].update(androidApi=34, expectedBuildSha256=receipt["device"]["buildSha256"])
    receipt["qualification"]["runtime"]["expectedVersion"] = dict(receipt["observed"]["openxr"]["runtimeVersion"])
    row["androidPreflight"]["receiptSha256"] = store(evidence, json.dumps(receipt).encode())
    result = run_matrix(release)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("path", [(), ("artifact",), ("device",), ("qualification", "runtime")])
def test_android_discovery_rejects_unclassified_nested_evidence_without_echoing_it(release, path):
    matrix, evidence = release
    row = matrix["tuples"][0]
    capture_android_smoke(row, evidence)
    receipt = android_receipt(row, evidence)
    nested = receipt
    for field in path:
        nested = nested[field]
    nested["privateCapture"] = {"room": "private-room-observation"}
    row["androidPreflight"]["receiptSha256"] = store(evidence, json.dumps(receipt).encode())
    result = run_matrix(release)
    assert result.returncode == 1
    assert "Unexpected fields in Android preflight" in result.stderr
    assert "privateCapture" not in result.stderr
    assert "private-room-observation" not in result.stderr + result.stdout


@pytest.mark.parametrize(("section", "key", "value", "message"), [
    ("artifact", "sourceRevision", "b" * 40, "another source revision"),
    ("artifact", "apkSha256", "a" * 64, "different diagnostic APK"),
    ("device", "buildSha256", "a" * 64, "different system build"),
    ("device", "abi", "x86_64", "different packaged ABI"),
    ("device", "androidApi", {"room": "private-room-observation"}, "Invalid Android preflight API level"),
    ("artifact", "applicationId", {"serial": "private-device"}, "Invalid Android preflight application"),
    ("qualification", "connection", {"requested": "usb", "observed": "tcpip"}, "different ADB connection"),
    ("qualification", "physicalXrQualification", "passed", "Expected cold discovery evidence"),
    ("observed", "crashObserved", True, "recorded a crash"),
    ("cleanup", "status", "unconfirmed", "process retirement is unconfirmed"),
])
def test_android_matrix_rejects_wrong_or_partial_discovery(release, section, key, value, message):
    matrix, evidence = release
    row = matrix["tuples"][0]
    capture_android_smoke(row, evidence)
    receipt = android_receipt(row, evidence)
    receipt[section][key] = value
    row["androidPreflight"]["receiptSha256"] = store(evidence, json.dumps(receipt).encode())
    result = run_matrix(release)
    assert result.returncode == 1
    assert message in result.stderr


def test_android_discovery_is_checked_against_the_runtime_version_in_the_smoke_tuple(release):
    matrix, evidence = release
    row = matrix["tuples"][0]
    capture_android_smoke(row, evidence, runtime_version="1.2.4")
    receipt = android_receipt(row, evidence)
    row["androidPreflight"]["receiptSha256"] = store(evidence, json.dumps(receipt).encode())
    result = run_matrix(release)
    assert result.returncode == 1
    assert "different runtime version" in result.stderr


@pytest.mark.parametrize(("expected", "message"), [
    ({"major": 1, "minor": 2, "patch": 4}, "expected a different runtime version"),
    ({"major": True, "minor": 2, "patch": 3}, "Invalid Android OpenXR runtime version"),
    ({"major": 1, "minor": 2, "patch": 2**32}, "Invalid Android OpenXR runtime version"),
    ("1.2.3", "Unexpected fields in Android OpenXR runtime version"),
])
def test_android_discovery_rejects_an_invalid_runtime_pin(release, expected, message):
    matrix, evidence = release
    row = matrix["tuples"][0]
    capture_android_smoke(row, evidence)
    receipt = android_receipt(row, evidence)
    receipt["qualification"]["runtime"]["expectedVersion"] = expected
    row["androidPreflight"]["receiptSha256"] = store(evidence, json.dumps(receipt).encode())
    result = run_matrix(release)
    assert result.returncode == 1
    assert message in result.stderr


@pytest.mark.parametrize(("extents", "message"), [
    ([{"width": 1024, "height": 768}], "Invalid Android stereo view count"),
    ([{"width": 1024, "height": 768}, {"width": True, "height": 800}], "Invalid Android stereo view extent"),
])
def test_android_discovery_rejects_invalid_cold_recommendations(release, extents, message):
    matrix, evidence = release
    row = matrix["tuples"][0]
    capture_android_smoke(row, evidence)
    receipt = android_receipt(row, evidence)
    receipt["observed"]["openxr"]["primaryStereoRecommendedViewExtents"] = extents
    row["androidPreflight"]["receiptSha256"] = store(evidence, json.dumps(receipt).encode())
    result = run_matrix(release)
    assert result.returncode == 1
    assert message in result.stderr


def test_android_preflight_does_not_apply_to_pcvr(release):
    matrix, evidence = release
    row = matrix["tuples"][1]
    receipt = android_receipt(row, evidence)
    row["androidPreflight"]["receiptSha256"] = store(evidence, json.dumps(receipt).encode())
    result = run_matrix(release)
    assert result.returncode == 1
    assert "Android preflight requires a standalone Android tuple" in result.stderr


def test_android_preflight_rejects_received_bytes_with_the_wrong_digest(release):
    matrix, evidence = release
    row = matrix["tuples"][0]
    capture_android_smoke(row, evidence)
    receipt = android_receipt(row, evidence)
    digest = store(evidence, json.dumps(receipt).encode())
    row["androidPreflight"]["receiptSha256"] = digest
    (evidence / digest).write_bytes(b"corrupt receipt")
    result = run_matrix(release)
    assert result.returncode == 1
    assert "Android preflight receipt digest does not match" in result.stderr


def desktop_receipt(row, *, host=None):
    identity = row["declaration"]["tuple"]
    system = f"{identity['runtime']}\0{identity['headsetModel']}".encode()
    target = hashlib.sha256(b"Horo Desktop OpenXR probe v1\0" + system).hexdigest()
    host = host or identity["hostPlatform"].lower()
    connection = {"Tethered": "tethered", "WirelessStreaming": "wireless-streaming"}[identity["connectionMode"]]
    return (f"horo-xr-desktop-qualification-v2 sha256:{COMPONENT} sha256:{identity['buildSha256']} "
            f"{host} system-default vulkan sha256:{target} {connection} preflight 1 2 3 1 3 0 2 2 3\n").encode()


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
