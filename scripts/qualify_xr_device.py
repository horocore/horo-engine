"""Check an XR smoke record and its eligibility for a declared physical tuple."""

from __future__ import annotations

import argparse
from collections.abc import Iterator
import hashlib
import json
from pathlib import Path
import re
from typing import NamedTuple


MAX_RECORD_BYTES = 64 * 1024
MAX_ARTIFACT_BYTES = 512 * 1024 * 1024
UTF8_BOM = b"\xef\xbb\xbf"
MAX_DESKTOP_PREFLIGHT_BYTES = 1024
MAX_DISPLAY_RATES = 32
MAX_TIMING_WINDOW_NS = 86_400_000_000_000
MAX_TIMING_FRAMES = 10_000_000
TIMING_LIMITS = {
    "minimumDurationNs": (1, MAX_TIMING_WINDOW_NS),
    "minimumFrameCount": (1, MAX_TIMING_FRAMES),
    "maximumPeakFrameIntervalNs": (1, MAX_TIMING_WINDOW_NS),
    "maximumMissedDeadlines": (0, MAX_TIMING_FRAMES),
    "maximumSwapchainFailures": (0, MAX_TIMING_FRAMES),
}
DESKTOP_PREFLIGHT_RECORD = "horo-xr-desktop-qualification-v2"
DESKTOP_DISPLAY_PREFLIGHT_RECORD = "horo-xr-desktop-qualification-v3"
DESKTOP_CONNECTIONS = {"Tethered": "tethered", "WirelessStreaming": "wireless-streaming"}
DESKTOP_PREFLIGHT_MAXIMA = (65535, 65535, 4294967295, 4294967295, 4294967295, 4294967295, 16, 128, 15)
DESKTOP_REQUIRED_ROUTES = 0b0011
DESKTOP_HOSTS = {"windows", "linux"}
DESKTOP_RENDERER = "vulkan"
CATEGORIES = {"Reference", "Compatibility", "Enterprise", "Conditional", "Future"}
LEVELS = {"StaticContract", "DeterministicFake", "SimulatorReplay",
          "RuntimeCompatibility", "PhysicalDeviceQualification"}
STATUSES = {"Passed", "PassedWithLimitations", "Failed", "Blocked", "NotRun"}
CONNECTION_MODES = {"Tethered", "WirelessStreaming", "Standalone"}
IDENTITY_FIELDS = (
    "buildSha256", "packageReceiptSha256", "headsetModel", "runtime", "runtimeVersion",
    "loaderVersion", "backendVersion", "hostPlatform", "hostVersion", "renderer",
    "driverClass", "driverVersion", "deviceClass", "connectionMode",
    "interactionProfile", "admittedExtensions",
)
CORE_CHECKS = {"setup", "input", "displayEnumeration", "renderTiming",
               "comfort", "lifecycle", "recovery"}
OPTIONAL_CHECKS = {"haptics", "optionalMR"}
ALL_CHECKS = CORE_CHECKS | OPTIONAL_CHECKS
SPECIALIZED_PHYSICAL_CHECKS = {"displayEnumeration", "renderTiming", "optionalMR"}
PHYSICAL_CHECK_RECEIPT_FIELDS = {"schemaVersion", "evidenceLevel", "tuple", "provenance", "check", "artifactSha256"}
OBSERVATION_IDENTITY_FIELDS = {"schemaVersion", "tuple", "provenance"}
MR_COMMON_CHECKS = {"operation", "consentDenial", "revocation", "fallback", "shutdown"}


class MixedRealityRoute(NamedTuple):
    extensions: frozenset[str]
    extra_checks: frozenset[str] = frozenset()


PLANE_TRACKING_EXTENSIONS = frozenset({"XR_EXT_future", "XR_EXT_spatial_entity", "XR_EXT_spatial_plane_tracking"})
MR_CAPABILITIES = {
    "passthrough": MixedRealityRoute(frozenset({"XR_FB_passthrough"})),
    "spatialAnchors": MixedRealityRoute(frozenset({"XR_MSFT_spatial_anchor"}), frozenset({"relocalization"})),
    "spatialAnchorPersistence": MixedRealityRoute(
        frozenset({"XR_MSFT_spatial_anchor", "XR_MSFT_spatial_anchor_persistence"}), frozenset({"restore", "staleLocalization"})),
    "scenePlanes": MixedRealityRoute(PLANE_TRACKING_EXTENSIONS),
    "environmentHitTesting": MixedRealityRoute(PLANE_TRACKING_EXTENSIONS),
    "lightEstimation": MixedRealityRoute(frozenset({"XR_ANDROID_light_estimation"})),
}
SHA256 = re.compile(r"[0-9a-f]{64}")
SOURCE_REVISION = re.compile(r"(?:[0-9a-f]{40}|[0-9a-f]{64})")
TOKEN = re.compile(r"[A-Za-z0-9][A-Za-z0-9 ._+/:@-]{0,127}")
EXTENSION = re.compile(r"XR_[A-Za-z0-9_]{1,125}")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def unique_members(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        require(key not in result, "Duplicate JSON field")
        result[key] = value
    return result


def decode_record(data: bytes) -> object:
    require(len(data) <= MAX_RECORD_BYTES, "Record exceeds the size limit")
    return json.loads(data.decode("utf-8"), object_pairs_hook=unique_members)


def load_record(path: Path) -> object:
    with path.open("rb") as source:
        return decode_record(source.read(MAX_RECORD_BYTES + 1))


def artifact_path(root: Path, digest: str) -> Path:
    require(is_digest(digest), "Missing or invalid artifact digest")
    path = root / digest
    require(path.is_file() and not path.is_symlink(), f"Missing regular artifact {digest}")
    return path


def verify_artifact(path: Path, digest: str, *, bounded_json: bool = False) -> bytes | None:
    """Hash an artifact, retaining small records and optionally bounding JSON observations."""
    require(is_digest(digest), "Missing or invalid artifact digest")
    require(path.is_file() and not path.is_symlink(), f"Missing regular artifact {digest}")
    hasher = hashlib.sha256()
    size = 0
    first_content = b""
    record_bytes = bytearray()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            if bounded_json and not first_content:
                prefix = chunk if size else chunk.removeprefix(UTF8_BOM)
                first_content = prefix.lstrip()[:1]
            size += len(chunk)
            require(size <= MAX_ARTIFACT_BYTES, f"Artifact exceeds the size budget: {digest}")
            if first_content == b"{":
                require(size <= MAX_RECORD_BYTES, "Physical observation JSON exceeds the size limit")
            hasher.update(chunk)
            if size <= MAX_RECORD_BYTES:
                record_bytes.extend(chunk)
    require(size > 0 and hasher.hexdigest() == digest, f"Artifact content does not match {digest}")
    return bytes(record_bytes) if size <= MAX_RECORD_BYTES else None


def fields(value: object, required: set[str], name: str) -> dict:
    require(isinstance(value, dict) and value.keys() == required, f"Unexpected fields in {name}")
    return value


def is_token(value: object) -> bool:
    return isinstance(value, str) and value == value.strip() and TOKEN.fullmatch(value) is not None


def is_digest(value: object) -> bool:
    return isinstance(value, str) and SHA256.fullmatch(value) is not None


def is_source_revision(value: object) -> bool:
    return isinstance(value, str) and SOURCE_REVISION.fullmatch(value) is not None


def in_set(value: object, choices: set[str]) -> bool:
    return isinstance(value, str) and value in choices


def validate_identity(value: object, *, complete: bool) -> dict:
    identity = fields(value, set(IDENTITY_FIELDS), "tuple")
    for key in IDENTITY_FIELDS[:-1]:
        entry = identity[key]
        if entry is None and not complete:
            continue
        if key == "connectionMode":
            require(in_set(entry, CONNECTION_MODES), "Invalid connection mode")
        elif key.endswith("Sha256"):
            require(is_digest(entry), f"Invalid digest in {key}")
        else:
            require(is_token(entry), f"Invalid identity in {key}")
    extensions = identity["admittedExtensions"]
    require(isinstance(extensions, list) and len(extensions) <= 128, "Invalid extension inventory")
    require(all(isinstance(name, str) and EXTENSION.fullmatch(name) for name in extensions),
            "Invalid OpenXR extension name")
    require(extensions == sorted(set(extensions)), "Extensions must be unique and sorted")
    return identity


def validate_check(value: object, name: str) -> None:
    check = fields(value, {"status", "evidenceSha256", "issue"}, f"{name} check")
    require(in_set(check["status"], STATUSES), f"Invalid {name} status")
    if check["status"] in {"Passed", "PassedWithLimitations", "Failed"}:
        require(is_digest(check["evidenceSha256"]), f"{name} requires observed evidence")
    elif check["evidenceSha256"] is not None:
        require(is_digest(check["evidenceSha256"]), f"Invalid {name} evidence digest")
    if check["status"] != "Passed":
        require(is_token(check["issue"]), f"{name} requires an issue or reason code")
    else:
        require(check["issue"] is None, f"Use PassedWithLimitations for a qualified {name} observation")


def validate_display(value: object) -> dict:
    display = fields(value, {"evidenceSha256", "refreshMilliHertz", "viewExtents"}, "display enumeration")
    require(is_digest(display["evidenceSha256"]), "Display must reference a runtime capture")
    validate_display_values(display)
    return display


def validate_display_values(display: dict) -> None:
    """Validate the bounded display values shared by the capture and smoke record."""
    refresh = display["refreshMilliHertz"]
    require(isinstance(refresh, list) and len(refresh) <= MAX_DISPLAY_RATES, "Invalid refresh inventory")
    require(all(type(rate) is int and 0 < rate <= 1_000_000 for rate in refresh),
            "Refresh values must be positive integer millihertz")
    require(refresh == sorted(set(refresh)), "Refresh values must be unique and sorted")
    views = display["viewExtents"]
    require(isinstance(views, list) and len(views) in {1, 2, 4}, "Invalid runtime view count")
    for view in views:
        extent = fields(view, {"width", "height"}, "view extent")
        require(all(type(size) is int and 0 < size <= 65536 for size in extent.values()), "Invalid runtime view extent")


def check_display_capture(data: bytes, record: dict) -> None:
    """Bind runtime display values to the exact physical smoke tuple and run."""
    display = record["display"]
    require(len(data) <= MAX_RECORD_BYTES, "Display capture exceeds the size limit")
    require(hashlib.sha256(data).hexdigest() == display["evidenceSha256"], "Display capture digest does not match")
    capture = fields(decode_record(data), {"schemaVersion", "tuple", "provenance", "refreshMilliHertz", "viewExtents"},
                     "display capture")
    require(type(capture["schemaVersion"]) is int and capture["schemaVersion"] == 1, "Unsupported display capture schema")
    validate_identity(capture["tuple"], complete=True)
    validate_provenance(capture["provenance"], capture["tuple"], observed=True)
    require(capture["tuple"] == record["tuple"], "Display capture is from a different product tuple")
    require(capture["provenance"] == record["provenance"], "Display capture is from a different smoke run")
    validate_display_values(capture)
    require(capture["refreshMilliHertz"] == display["refreshMilliHertz"] and capture["viewExtents"] == display["viewExtents"],
            "Display report does not match the captured runtime values")


def validate_timing_limits(value: object) -> None:
    limits = fields(value, set(TIMING_LIMITS), "render timing limits")
    require(all(type(limits[name]) is int and lower <= limits[name] <= upper
                for name, (lower, upper) in TIMING_LIMITS.items()), "Invalid render timing limits")


def check_render_timing(data: bytes, record: dict, limits: dict) -> None:
    """Verify a bounded timing observation from the exact physical run against its release limits."""
    require(len(data) <= MAX_RECORD_BYTES, "Render timing capture exceeds the size limit")
    digest = record["checks"]["renderTiming"]["evidenceSha256"]
    require(hashlib.sha256(data).hexdigest() == digest, "Render timing capture digest does not match")
    capture = fields(decode_record(data), {"schemaVersion", "tuple", "provenance", "targetRefreshMilliHertz",
                                           "durationNs", "frameCount", "peakFrameIntervalNs", "missedDeadlines",
                                           "swapchainFailures"}, "render timing capture")
    require(type(capture["schemaVersion"]) is int and capture["schemaVersion"] == 1, "Unsupported render timing capture schema")
    require(capture["tuple"] == record["tuple"], "Render timing capture is from a different product tuple")
    require(capture["provenance"] == record["provenance"], "Render timing capture is from a different smoke run")
    refresh = capture["targetRefreshMilliHertz"]
    require(type(refresh) is int and refresh in record["display"]["refreshMilliHertz"], "Render timing target was not enumerated")
    duration, frames, peak, missed, failures = (capture[name] for name in (
        "durationNs", "frameCount", "peakFrameIntervalNs", "missedDeadlines", "swapchainFailures"))
    require(all(type(value) is int for value in (duration, frames, peak, missed, failures)) and
            0 < peak <= duration <= MAX_TIMING_WINDOW_NS and
            0 < frames <= MAX_TIMING_FRAMES and 0 <= missed <= frames and 0 <= failures <= frames,
            "Invalid render timing measurements")
    require(peak + frames - 1 <= duration <= frames * peak, "Render timing interval totals are inconsistent")
    require(duration >= limits["minimumDurationNs"] and frames >= limits["minimumFrameCount"],
            "Render timing observation did not meet the required duration and frame count")
    require(peak <= limits["maximumPeakFrameIntervalNs"] and missed <= limits["maximumMissedDeadlines"] and
            failures <= limits["maximumSwapchainFailures"], "Render timing observation exceeded the declared limits")


def check_render_timing_file(path: Path, record: dict, limits: dict) -> None:
    """Read a bounded timing capture against the smoke record and validated limits."""
    with path.open("rb") as source:
        check_render_timing(source.read(MAX_RECORD_BYTES + 1), record, limits)


def validate_summary(status: str, checks: dict) -> None:
    """Reject aggregate outcomes that contradict the individual observations."""
    check_statuses = {check["status"] for check in checks.values()}
    if status == "Passed":
        require(check_statuses == {"Passed"}, "A fully passed record requires every check to pass")
    elif status == "PassedWithLimitations":
        require("Failed" not in check_statuses, "A failed check requires a Failed record")
        require(bool(check_statuses & {"Passed", "PassedWithLimitations"}),
                "A record passed with limitations requires a passing observation")
        require(check_statuses != {"Passed"}, "Record the limitation in the affected check")
    elif status == "NotRun":
        require(check_statuses == {"NotRun"}, "A NotRun record cannot contain completed checks")


def validate_display_check(value: object, check: dict) -> None:
    """Bind enumerated display data to the capture that supports the check."""
    result = check["status"]
    if result in {"Passed", "PassedWithLimitations"}:
        display = validate_display(value)
        require(display["evidenceSha256"] == check["evidenceSha256"],
                "Display data must reference the display check's runtime capture")
        if result == "Passed":
            require(bool(display["refreshMilliHertz"]), "A passed display check requires enumerated refresh rates")
    else:
        require(value is None, "Display data requires a completed runtime enumeration")


def is_standalone_android(identity: dict) -> bool:
    """Identify the Android system build's physical standalone route."""
    return identity["connectionMode"] == "Standalone" and (identity["hostPlatform"] or "").lower() == "android"


def validate_provenance(value: object, identity: dict, *, observed: bool) -> None:
    """Validate the test identity and any independently captured Android system build."""
    keys = {"testPlan", "runId"}
    if isinstance(value, dict):
        keys |= value.keys() & {"sourceRevision", "androidDeviceBuildSha256"}
    provenance = fields(value, keys, "provenance")
    if "sourceRevision" in provenance:
        require(is_source_revision(provenance["sourceRevision"]), "Invalid device evidence source revision")
    if "androidDeviceBuildSha256" in provenance:
        require(is_digest(provenance["androidDeviceBuildSha256"]), "Invalid Android system build digest")
        require(is_standalone_android(identity), "Android system build digest requires a standalone Android tuple")
    require(is_token(provenance["testPlan"]), "Missing test-plan revision")
    require(is_token(provenance["runId"]) if observed else provenance["runId"] is None or is_token(provenance["runId"]),
            "Missing or invalid provenance run identity")


def validate_record(value: object) -> dict:
    record = fields(value, {"schemaVersion", "category", "evidenceLevel", "status", "tuple",
                            "provenance", "checks", "display"}, "record")
    require(type(record["schemaVersion"]) is int and record["schemaVersion"] == 1, "Unsupported XR evidence schema")
    require(in_set(record["category"], CATEGORIES), "Invalid planning category")
    require(in_set(record["evidenceLevel"], LEVELS), "Invalid evidence level")
    require(in_set(record["status"], STATUSES), "Invalid record status")
    observed = record["status"] not in {"Blocked", "NotRun"}
    validate_identity(record["tuple"], complete=observed and record["evidenceLevel"] == "PhysicalDeviceQualification")
    validate_provenance(record["provenance"], record["tuple"], observed=observed)
    checks = fields(record["checks"], ALL_CHECKS, "checks")
    for name, check in checks.items():
        validate_check(check, name)
    validate_summary(record["status"], checks)
    validate_display_check(record["display"], checks["displayEnumeration"])
    return record


def validate_declaration(value: object, optional: set[str]) -> dict:
    """Validate an exact tuple and the scope of any optional MR claim."""
    claims_mr = "optionalMR" in optional
    keys = {"tuple", "testPlan"}
    if claims_mr:
        keys.add("mixedRealityCapabilities")
    expected = fields(value, keys, "release declaration")
    require(is_token(expected["testPlan"]), "Missing required test-plan revision")
    validate_identity(expected["tuple"], complete=True)
    if claims_mr:
        capabilities = expected["mixedRealityCapabilities"]
        require(isinstance(capabilities, list) and 0 < len(capabilities) <= len(MR_CAPABILITIES),
                "An optionalMR claim requires declared MR capabilities")
        require(all(isinstance(name, str) and name in MR_CAPABILITIES for name in capabilities),
                "Invalid declared MR capability")
        require(len(set(capabilities)) == len(capabilities), "Duplicate declared MR capability")
        admitted = set(expected["tuple"]["admittedExtensions"])
        for capability in capabilities:
            missing = MR_CAPABILITIES[capability].extensions - admitted
            require(not missing, f"Declared {capability} capability is missing admitted OpenXR extensions {', '.join(sorted(missing))}")
    return expected


def check_mixed_reality_capture(data: bytes, record: dict, capabilities: list[str]) -> None:
    """Accept only redacted, complete per-capability evidence from the physical smoke run."""
    require(len(data) <= MAX_RECORD_BYTES, "Mixed reality capture exceeds the size limit")
    require(hashlib.sha256(data).hexdigest() == record["checks"]["optionalMR"]["evidenceSha256"],
            "Mixed reality capture digest does not match")
    try:
        document = decode_record(data)
    except (ValueError, RecursionError) as error:
        raise ValueError("Invalid mixed reality capture JSON") from error
    capture = fields(document, {"schemaVersion", "evidenceLevel", "tuple", "provenance", "capabilities"},
                     "mixed reality capture")
    require(type(capture["schemaVersion"]) is int and capture["schemaVersion"] == 1, "Unsupported mixed reality capture schema")
    require(capture["evidenceLevel"] == "PhysicalDeviceQualification", "Mixed reality capture requires physical headset evidence")
    require(capture["tuple"] == record["tuple"], "Mixed reality capture is from a different product tuple")
    require(capture["provenance"] == record["provenance"], "Mixed reality capture is from a different smoke run")
    results = fields(capture["capabilities"], set(capabilities), "mixed reality capabilities")
    for name, observed in results.items():
        checks = fields(observed, MR_COMMON_CHECKS | MR_CAPABILITIES[name].extra_checks, f"{name} evidence")
        require(all(value is True for value in checks.values()), f"Required {name} scenario did not pass")


def check_physical_gate(record: dict, declaration: object, optional: set[str], *, mr_capture: bytes | None = None) -> None:
    expected = validate_declaration(declaration, optional)
    require(record["evidenceLevel"] == "PhysicalDeviceQualification", "Physical headset evidence is required")
    require(record["tuple"] == expected["tuple"], "Evidence does not match the declared product tuple")
    require(record["provenance"]["testPlan"] == expected["testPlan"], "Evidence uses a different test plan")
    require(record["status"] in {"Passed", "PassedWithLimitations"}, "Physical smoke record did not pass")
    for name in sorted(CORE_CHECKS | optional):
        require(record["checks"][name]["status"] == "Passed", f"Required physical check {name} did not pass")
    if "optionalMR" in optional:
        require(mr_capture is not None, "Required mixed reality capture was not provided")
        check_mixed_reality_capture(mr_capture, record, expected["mixedRealityCapabilities"])


def physical_check_capture(path: Path, record: dict, check: str) -> str:
    """Verify the exact physical check receipt and return its redacted observation digest."""
    with path.open("rb") as source:
        data = source.read(MAX_RECORD_BYTES + 1)
    require(len(data) <= MAX_RECORD_BYTES, f"{check} capture exceeds the size limit")
    digest = record["checks"][check]["evidenceSha256"]
    require(hashlib.sha256(data).hexdigest() == digest, f"{check} capture digest does not match")
    capture = fields(decode_record(data), PHYSICAL_CHECK_RECEIPT_FIELDS, f"{check} capture")
    require(type(capture["schemaVersion"]) is int and capture["schemaVersion"] == 1, f"Unsupported {check} capture schema")
    require(capture["evidenceLevel"] == record["evidenceLevel"], f"{check} capture must identify physical evidence")
    require(capture["check"] == check, f"{check} capture is for a different check")
    require(capture["tuple"] == record["tuple"], f"{check} capture is from a different product tuple")
    require(capture["provenance"] == record["provenance"], f"{check} capture is from a different smoke run")
    artifact = capture["artifactSha256"]
    require(is_digest(artifact), "Missing or invalid artifact digest")
    return artifact


def required_physical_check_receipts(record: dict, optional: set[str]) -> Iterator[tuple[str, str]]:
    """List the required checks whose captures use the generic receipt format."""
    for check in sorted((CORE_CHECKS | optional) - SPECIALIZED_PHYSICAL_CHECKS):
        yield check, record["checks"][check]["evidenceSha256"]


def physical_check_evidence(root: Path, record: dict, optional: set[str]) -> tuple[set[str], set[str]]:
    """Verify receipt identities and return the distinct receipt and observation digests."""
    receipts = tuple(required_physical_check_receipts(record, optional))
    receipt_digests = {digest for _, digest in receipts}
    observations = set()
    for check, digest in receipts:
        artifact = physical_check_capture(artifact_path(root, digest), record, check)
        require(artifact not in receipt_digests, f"{check} observation refers to a physical check receipt")
        observations.add(artifact)
    return receipt_digests, observations


def verify_physical_observations(root: Path, record: dict, observations: set[str]) -> None:
    """Validate direct observations, including embedded qualification identities."""
    product_artifacts = {record["tuple"]["buildSha256"], record["tuple"]["packageReceiptSha256"]}
    for artifact in sorted(observations):
        require(artifact not in product_artifacts, "Physical observation refers to a product build or package receipt")
        data = verify_artifact(artifact_path(root, artifact), artifact, bounded_json=True)
        if data is None:
            continue
        candidate = data.removeprefix(UTF8_BOM)
        if not candidate.lstrip().startswith(b"{"):
            continue
        try:
            capture = decode_record(candidate)
        except (ValueError, RecursionError) as error:
            raise ValueError("Invalid physical observation JSON") from error
        if {"tuple", "provenance", "evidenceLevel"}.isdisjoint(capture):
            continue
        require(not PHYSICAL_CHECK_RECEIPT_FIELDS <= capture.keys(),
                "Physical observation refers to a physical check receipt")
        require(OBSERVATION_IDENTITY_FIELDS <= capture.keys(), "Physical observation is missing qualification identity")
        require(type(capture["schemaVersion"]) is int and capture["schemaVersion"] == 1,
                "Unsupported physical observation schema")
        require(capture.get("evidenceLevel", record["evidenceLevel"]) == record["evidenceLevel"],
                "Physical observation must identify physical evidence")
        require(capture["tuple"] == record["tuple"] and capture["provenance"] == record["provenance"],
                "Physical observation capture is from a different product tuple or smoke run")


def verify_physical_checks(root: Path, record: dict, optional: set[str]) -> None:
    """Validate each receipt and its referenced bytes in the selected evidence directory."""
    _, observations = physical_check_evidence(root, record, optional)
    verify_physical_observations(root, record, observations)


def desktop_preflight_facts(words: list[str]) -> tuple[int, ...]:
    require(len(words) == len(DESKTOP_PREFLIGHT_MAXIMA), "Invalid Desktop preflight facts")
    require(all(re.fullmatch(r"[0-9]{1,10}", word) for word in words), "Invalid Desktop preflight fact")
    facts = tuple(map(int, words))
    require(all(value <= maximum for value, maximum in zip(facts, DESKTOP_PREFLIGHT_MAXIMA)),
            "Desktop preflight fact exceeds the supported range")
    runtime_major, _, _, graphics_major, _, _, spaces, formats, routes = facts
    require(runtime_major > 0 and graphics_major > 0 and spaces >= 2 and formats > 0,
            "Desktop preflight lacks required runtime or renderer evidence")
    require(routes & DESKTOP_REQUIRED_ROUTES == DESKTOP_REQUIRED_ROUTES,
            "Desktop preflight lacks a required provider route")
    return facts


def desktop_receipt_digest(package_receipt: str, preflight: str | None, component: str | None) -> str:
    """Resolve the expected receipt digest and require independent identity for a separate receipt."""
    digest = package_receipt if preflight is None else preflight
    require(is_digest(digest), "Invalid Desktop preflight digest")
    require(component is None or is_digest(component), "Invalid Desktop component digest")
    if digest != package_receipt:
        require(component is not None, "Separate Desktop preflight requires the package component digest")
    return digest


def check_desktop_display_field(field: str) -> int:
    """Validate the v3 measurement suffix and return its runtime view count."""
    require(field.startswith("display="), "Missing Desktop display measurements")
    tokens = field.removeprefix("display=").split(",")
    require(all(re.fullmatch(r"0|[1-9][0-9]{0,6}", token) for token in tokens), "Invalid Desktop display measurements")
    rate_count, *values = map(int, tokens)
    require(rate_count <= MAX_DISPLAY_RATES and len(values) > rate_count, "Invalid Desktop display rate count")
    view_count = values[rate_count]
    extents = values[rate_count + 1:]
    require(view_count in {2, 4} and len(extents) == 2 * view_count, "Invalid Desktop display view count")
    validate_display_values({"refreshMilliHertz": values[:rate_count],
                             "viewExtents": [{"width": width, "height": height} for width, height in zip(extents[::2], extents[1::2])]})
    return view_count


def check_desktop_preflight(data: bytes, identity: dict, receipt_digest: str,
                            component_digest: str | None = None) -> int | None:
    """Match a retired package probe to its tuple and return the view count when measured."""
    require(0 < len(data) <= MAX_DESKTOP_PREFLIGHT_BYTES, "Desktop preflight exceeds the size limit")
    require(hashlib.sha256(data).hexdigest() == receipt_digest,
            "Desktop preflight does not match the declared receipt digest")
    require(data.endswith(b"\n"), "Invalid Desktop preflight line ending")
    columns = data[:-1].removesuffix(b"\r").decode("ascii").split(" ")
    has_display = columns[0] == DESKTOP_DISPLAY_PREFLIGHT_RECORD
    require(len(columns) == (19 if has_display else 18), "Invalid Desktop preflight receipt")
    record, install, executable, host, route, renderer, target, connection, outcome, *facts = columns
    require((record == DESKTOP_PREFLIGHT_RECORD or has_display) and outcome == "preflight", "Expected a successful Desktop preflight")
    require(install.startswith("sha256:") and is_digest(install[7:]), "Invalid Desktop preflight installation digest")
    if component_digest is not None:
        require(install == f"sha256:{component_digest}", "Desktop preflight uses a different installed component")
    require(executable == f"sha256:{identity['buildSha256']}", "Desktop preflight uses a different executable")
    require(host in DESKTOP_HOSTS and host == identity["hostPlatform"].lower(), "Desktop preflight uses a different host")
    require(route == "system-default" and renderer == DESKTOP_RENDERER and renderer == identity["renderer"].lower(),
            "Desktop preflight uses a different runtime route or renderer")
    expected_connection = DESKTOP_CONNECTIONS.get(identity["connectionMode"])
    require(expected_connection is not None, "Desktop preflight requires a PCVR declaration")
    require(connection == expected_connection, "Desktop preflight uses a different connection mode")
    labels = f"{identity['runtime']}\0{identity['headsetModel']}".encode()
    target_digest = hashlib.sha256(b"Horo Desktop OpenXR probe v1\0" + labels).hexdigest()
    require(target == f"sha256:{target_digest}", "Desktop preflight uses a different runtime or system")
    view_count = check_desktop_display_field(facts.pop()) if has_display else None
    runtime_version = ".".join(map(str, desktop_preflight_facts(facts)[:3]))
    require(runtime_version == identity["runtimeVersion"], "Desktop preflight uses a different runtime version")
    return view_count


def check_desktop_preflight_file(path: Path, identity: dict, preflight: str | None, component: str | None) -> None:
    """Verify either the package receipt itself or an independently identified preflight."""
    digest = desktop_receipt_digest(identity["packageReceiptSha256"], preflight, component)
    with path.open("rb") as receipt:
        check_desktop_preflight(receipt.read(MAX_DESKTOP_PREFLIGHT_BYTES + 1), identity, digest, component)


def parse_options(parser: argparse.ArgumentParser) -> argparse.Namespace:
    """Parse an invocation and validate the relationships between receipt inputs."""
    parser.add_argument("record", type=Path)
    parser.add_argument("--expect", type=Path, help="Exact release tuple and test-plan declaration")
    parser.add_argument("--physical-check-evidence-dir", type=Path, help="Directory of SHA-256-named physical check receipts and observations")
    parser.add_argument("--display-capture", type=Path, help="Runtime display capture for the exact physical smoke run")
    parser.add_argument("--render-timing-capture", type=Path, help="Timing capture for the exact physical smoke run")
    parser.add_argument("--render-timing-limits", type=Path, help="Independent render timing limits from the release plan")
    parser.add_argument("--desktop-preflight", type=Path, help="Exact packaged Desktop preflight receipt for the declared tuple")
    parser.add_argument("--desktop-preflight-sha256", help="Expected SHA-256 for a separate Desktop preflight receipt")
    parser.add_argument("--desktop-component-sha256", help="Installed OpenXR component SHA-256 from package provenance")
    parser.add_argument("--android-preflight", type=Path, help="Redacted Android OpenXR cold-discovery receipt")
    parser.add_argument("--android-preflight-declaration", type=Path, help="Independent Android preflight declaration")
    parser.add_argument("--android-apk", type=Path, help="Exact diagnostic APK identified by the Android preflight declaration")
    parser.add_argument("--require", choices=sorted(OPTIONAL_CHECKS), action="append", default=[])
    parser.add_argument("--mr-capture", type=Path, help="Redacted physical MR capability evidence for --require optionalMR")
    options = parser.parse_args()
    if options.require and options.expect is None:
        parser.error("--require needs --expect")
    if options.physical_check_evidence_dir is not None and options.expect is None:
        parser.error("--physical-check-evidence-dir needs --expect")
    if options.display_capture is not None and options.expect is None:
        parser.error("--display-capture needs --expect")
    timing = (options.render_timing_capture, options.render_timing_limits)
    if any(timing) and (not all(timing) or options.display_capture is None):
        parser.error("Render timing requires --render-timing-capture, --render-timing-limits and --display-capture")
    if (options.mr_capture is not None) != ("optionalMR" in options.require):
        parser.error("--mr-capture and --require optionalMR must be used together")
    if options.desktop_preflight is not None and options.expect is None:
        parser.error("--desktop-preflight needs --expect")
    if options.desktop_preflight is None and (options.desktop_preflight_sha256 is not None or options.desktop_component_sha256 is not None):
        parser.error("Desktop receipt digest options need --desktop-preflight")
    android = (options.android_preflight, options.android_preflight_declaration, options.android_apk)
    if any(android) and (not all(android) or options.expect is None):
        parser.error("Android preflight requires --expect, --android-preflight, --android-preflight-declaration, and --android-apk")
    return options


def validate_android_capture(options: argparse.Namespace, record: dict) -> None:
    """Verify the diagnostic artifact and receipt against the local physical smoke record."""
    from qualify_xr_android import check_android_preflight_file, validate_android_preflight

    expected = validate_android_preflight(load_record(options.android_preflight_declaration))
    verify_artifact(options.android_apk, expected["apkSha256"])
    check_android_preflight_file(options.android_preflight, options.android_apk, expected, record)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    options = parse_options(parser)
    try:
        record = validate_record(load_record(options.record))
        if options.expect is not None:
            mr_capture = None
            if options.mr_capture is not None:
                with options.mr_capture.open("rb") as source:
                    mr_capture = source.read(MAX_RECORD_BYTES + 1)
            check_physical_gate(record, load_record(options.expect), set(options.require), mr_capture=mr_capture)
        if options.physical_check_evidence_dir is not None:
            verify_physical_checks(options.physical_check_evidence_dir, record, set(options.require))
        if options.display_capture is not None:
            with options.display_capture.open("rb") as source:
                check_display_capture(source.read(MAX_RECORD_BYTES + 1), record)
        if options.render_timing_capture is not None:
            limits = load_record(options.render_timing_limits)
            validate_timing_limits(limits)
            check_render_timing_file(options.render_timing_capture, record, limits)
        if options.desktop_preflight is not None:
            check_desktop_preflight_file(options.desktop_preflight, record["tuple"], options.desktop_preflight_sha256,
                                         options.desktop_component_sha256)
        if options.android_preflight is not None:
            validate_android_capture(options, record)
    except (OSError, ValueError, RecursionError) as error:
        parser.exit(1, f"XR qualification record rejected: {error}\n")
    print("Physical smoke evidence matches the declaration" if options.expect else "XR evidence record is valid")
    if options.physical_check_evidence_dir is not None:
        print("Physical check receipts and their observations match the smoke run")
    if options.display_capture is not None:
        print("Runtime display capture matches the physical smoke run")
    if options.render_timing_capture is not None:
        print("Runtime timing capture meets the declared limits")
    if options.android_preflight is not None:
        print("Android OpenXR preflight matches the physical smoke run")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
