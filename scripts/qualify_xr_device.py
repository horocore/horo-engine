"""Check an XR smoke record and its eligibility for a declared physical tuple."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re


MAX_RECORD_BYTES = 64 * 1024
MAX_DESKTOP_PREFLIGHT_BYTES = 1024
MAX_DISPLAY_RATES = 32
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


def check_desktop_display_field(field: str) -> None:
    """Validate the complete bounded measurement suffix from a v3 packaged probe."""
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


def check_desktop_preflight(data: bytes, identity: dict, receipt_digest: str,
                            component_digest: str | None = None) -> None:
    """Match a retired Desktop package probe to the physical tuple it can inform."""
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
    if has_display:
        check_desktop_display_field(facts.pop())
    runtime_version = ".".join(map(str, desktop_preflight_facts(facts)[:3]))
    require(runtime_version == identity["runtimeVersion"], "Desktop preflight uses a different runtime version")


def check_desktop_preflight_file(path: Path, identity: dict, preflight: str | None, component: str | None) -> None:
    """Verify either the package receipt itself or an independently identified preflight."""
    digest = desktop_receipt_digest(identity["packageReceiptSha256"], preflight, component)
    with path.open("rb") as receipt:
        check_desktop_preflight(receipt.read(MAX_DESKTOP_PREFLIGHT_BYTES + 1), identity, digest, component)


def parse_options(parser: argparse.ArgumentParser) -> argparse.Namespace:
    """Parse an invocation and validate the relationships between receipt inputs."""
    parser.add_argument("record", type=Path)
    parser.add_argument("--expect", type=Path, help="Exact release tuple and test-plan declaration")
    parser.add_argument("--desktop-preflight", type=Path, help="Exact packaged Desktop preflight receipt for the declared tuple")
    parser.add_argument("--desktop-preflight-sha256", help="Expected SHA-256 for a separate Desktop preflight receipt")
    parser.add_argument("--desktop-component-sha256", help="Installed OpenXR component SHA-256 from package provenance")
    parser.add_argument("--require", choices=sorted(OPTIONAL_CHECKS), action="append", default=[])
    options = parser.parse_args()
    if options.require and options.expect is None:
        parser.error("--require needs --expect")
    if options.desktop_preflight is not None and options.expect is None:
        parser.error("--desktop-preflight needs --expect")
    if options.desktop_preflight is None and (options.desktop_preflight_sha256 is not None or options.desktop_component_sha256 is not None):
        parser.error("Desktop receipt digest options need --desktop-preflight")
    return options


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    options = parse_options(parser)
    try:
        record = validate_record(load_record(options.record))
        if options.expect is not None:
            check_physical_gate(record, load_record(options.expect), set(options.require))
        if options.desktop_preflight is not None:
            check_desktop_preflight_file(options.desktop_preflight, record["tuple"], options.desktop_preflight_sha256,
                                         options.desktop_component_sha256)
    except (OSError, ValueError, RecursionError) as error:
        parser.exit(1, f"XR qualification record rejected: {error}\n")
    print("Physical smoke evidence matches the declaration" if options.expect else "XR evidence record is valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
