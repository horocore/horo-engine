"""Check an XR smoke record and its eligibility for a declared physical tuple."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re


MAX_RECORD_BYTES = 64 * 1024
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
SHA256 = re.compile(r"[0-9a-f]{64}")
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


def fields(value: object, required: set[str], name: str) -> dict:
    require(isinstance(value, dict) and value.keys() == required, f"Unexpected fields in {name}")
    return value


def is_token(value: object) -> bool:
    return isinstance(value, str) and value == value.strip() and TOKEN.fullmatch(value) is not None


def is_digest(value: object) -> bool:
    return isinstance(value, str) and SHA256.fullmatch(value) is not None


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


def validate_display(value: object) -> None:
    display = fields(value, {"evidenceSha256", "refreshMilliHertz", "viewExtents"}, "display enumeration")
    require(is_digest(display["evidenceSha256"]), "Display must reference a runtime capture")
    refresh = display["refreshMilliHertz"]
    require(isinstance(refresh, list) and len(refresh) <= 32, "Invalid refresh inventory")
    require(all(type(rate) is int and 0 < rate <= 1_000_000 for rate in refresh),
            "Refresh values must be positive integer millihertz")
    require(refresh == sorted(set(refresh)), "Refresh values must be unique and sorted")
    views = display["viewExtents"]
    require(isinstance(views, list) and len(views) in {1, 2, 4}, "Invalid runtime view count")
    for view in views:
        extent = fields(view, {"width", "height"}, "view extent")
        require(all(type(size) is int and 0 < size <= 65536 for size in extent.values()), "Invalid runtime view extent")


def validate_record(value: object) -> dict:
    record = fields(value, {"schemaVersion", "category", "evidenceLevel", "status", "tuple",
                            "provenance", "checks", "display"}, "record")
    require(type(record["schemaVersion"]) is int and record["schemaVersion"] == 1, "Unsupported XR evidence schema")
    require(in_set(record["category"], CATEGORIES), "Invalid planning category")
    require(in_set(record["evidenceLevel"], LEVELS), "Invalid evidence level")
    require(in_set(record["status"], STATUSES), "Invalid record status")
    observed = record["status"] not in {"Blocked", "NotRun"}
    validate_identity(record["tuple"], complete=observed and record["evidenceLevel"] == "PhysicalDeviceQualification")
    provenance = fields(record["provenance"], {"testPlan", "runId"}, "provenance")
    require(is_token(provenance["testPlan"]), "Missing test-plan revision")
    require(is_token(provenance["runId"]) if observed else provenance["runId"] is None or is_token(provenance["runId"]),
            "Missing or invalid provenance run identity")
    checks = fields(record["checks"], ALL_CHECKS, "checks")
    for name, check in checks.items():
        validate_check(check, name)
    check_statuses = {check["status"] for check in checks.values()}
    if record["status"] == "Passed":
        require(check_statuses == {"Passed"}, "A fully passed record requires every check to pass")
    if record["status"] == "PassedWithLimitations":
        require(check_statuses != {"Passed"}, "Record the limitation in the affected check")
    if record["status"] == "NotRun":
        require(check_statuses == {"NotRun"}, "A NotRun record cannot contain completed checks")
    result = checks["displayEnumeration"]["status"]
    if result in {"Passed", "PassedWithLimitations"}:
        validate_display(record["display"])
        require(record["display"]["evidenceSha256"] == checks["displayEnumeration"]["evidenceSha256"],
                "Display data must reference the display check's runtime capture")
        if result == "Passed":
            require(bool(record["display"]["refreshMilliHertz"]), "A passed display check requires enumerated refresh rates")
    else:
        require(record["display"] is None, "Display data requires a completed runtime enumeration")
    return record


def check_physical_gate(record: dict, declaration: object, optional: set[str]) -> None:
    expected = fields(declaration, {"tuple", "testPlan"}, "release declaration")
    require(is_token(expected["testPlan"]), "Missing required test-plan revision")
    identity = validate_identity(expected["tuple"], complete=True)
    require(record["evidenceLevel"] == "PhysicalDeviceQualification", "Physical headset evidence is required")
    require(record["tuple"] == identity, "Evidence does not match the declared product tuple")
    require(record["provenance"]["testPlan"] == expected["testPlan"], "Evidence uses a different test plan")
    require(record["status"] in {"Passed", "PassedWithLimitations"}, "Physical smoke record did not pass")
    for name in sorted(CORE_CHECKS | optional):
        require(record["checks"][name]["status"] == "Passed", f"Required physical check {name} did not pass")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("record", type=Path)
    parser.add_argument("--expect", type=Path, help="Exact release tuple and test-plan declaration")
    parser.add_argument("--require", choices=sorted(OPTIONAL_CHECKS), action="append", default=[])
    options = parser.parse_args()
    if options.require and options.expect is None:
        parser.error("--require needs --expect")
    try:
        record = validate_record(load_record(options.record))
        if options.expect is not None:
            check_physical_gate(record, load_record(options.expect), set(options.require))
    except (OSError, ValueError, RecursionError) as error:
        parser.exit(1, f"XR qualification record rejected: {error}\n")
    print("Physical smoke evidence matches the declaration" if options.expect else "XR evidence record is valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
