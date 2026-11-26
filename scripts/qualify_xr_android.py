"""Validate bounded Android cold-discovery receipts referenced by an XR release row."""

from __future__ import annotations

import hashlib
from pathlib import Path

from project import OPENXR_DEVELOPMENT_PROFILES, android_profile
from qualify_xr_device import decode_record, fields, in_set, is_digest, is_standalone_android, require


MAX_ANDROID_RECEIPT_BYTES = 16 * 1024
DEPLOY_HOSTS = {"linux-x64", "macos-arm64", "macos-x64", "windows-x64"}
ANDROID_PREFLIGHT_FIELDS = {"receiptSha256", "apkSha256", "deviceBuildSha256", "profile", "adbConnection"}
ANDROID_RECEIPT_FIELDS = {"schema", "status", "stage", "artifact", "qualification", "device", "observed", "cleanup"}


def receipt_fields(value: object, required: set[str], optional: set[str], name: str) -> dict:
    """Allow only fields from the redacted deployment schema, including declared optional evidence."""
    require(isinstance(value, dict) and required <= value.keys() <= required | optional, f"Unexpected fields in {name}")
    return value


def is_json_integer(value: object) -> bool:
    """Exclude JSON booleans from integer fields."""
    return isinstance(value, int) and not isinstance(value, bool)


def android_runtime_version(value: object) -> str:
    """Normalize the deployer's exact OpenXR version fields for comparison with a release tuple."""
    version = fields(value, {"major", "minor", "patch"}, "Android OpenXR runtime version")
    parts = (version["major"], version["minor"], version["patch"])
    require(all(is_json_integer(part) and low <= part < high for part, low, high in
                zip(parts, (1, 0, 0), (2**16, 2**16, 2**32))), "Invalid Android OpenXR runtime version")
    return ".".join(map(str, parts))


def validate_android_preflight(value: object) -> dict:
    """Read an independently selected diagnostic package and Android target."""
    expected = fields(value, ANDROID_PREFLIGHT_FIELDS, "Android preflight declaration")
    for name in ("receiptSha256", "apkSha256", "deviceBuildSha256"):
        require(is_digest(expected[name]), f"Invalid Android preflight {name}")
    name = expected["profile"]
    require(isinstance(name, str) and name in OPENXR_DEVELOPMENT_PROFILES, "Unsupported Android preflight profile")
    require(in_set(expected["adbConnection"], {"usb", "tcpip"}), "Android preflight requires a physical ADB connection")
    return expected


def check_android_artifact(artifact: object, expected: dict, profile: dict, revision: str, abi: str) -> None:
    artifact = receipt_fields(artifact, {"apkSha256", "sourceRevision", "profile", "provenanceSha256", "openxr", "openxrImages"},
                              {"expectedApkSha256", "applicationId", "declaredRenderer", "abis"}, "Android preflight artifact")
    require(artifact.get("apkSha256") == expected["apkSha256"], "Android preflight uses a different diagnostic APK")
    require(artifact.get("sourceRevision") == revision, "Android preflight is from another source revision")
    require(artifact.get("profile") == expected["profile"], "Android preflight uses a different diagnostic profile")
    route = artifact.get("openxr")
    require(route == profile["openxrProfile"] and is_json_integer(route["schema"]) and is_digest(artifact.get("provenanceSha256")),
            "Invalid Android preflight artifact provenance")
    images = fields(artifact.get("openxrImages"), {"abi", "diagnosticHostSha256", "loaderSha256"}, "Android preflight images")
    require(images["abi"] == abi, "Android preflight uses a different packaged ABI")
    require(is_digest(images["diagnosticHostSha256"]) and is_digest(images["loaderSha256"]),
            "Invalid Android preflight image digests")
    require(artifact.get("expectedApkSha256", expected["apkSha256"]) == expected["apkSha256"],
            "Android preflight expected a different diagnostic APK")
    application = profile["applicationId"]
    require(artifact.get("applicationId", application) == application, "Invalid Android preflight application")
    renderer = profile["renderer"]
    require(artifact.get("declaredRenderer", renderer) == renderer, "Invalid Android cold discovery renderer")
    abis = artifact.get("abis", [abi])
    supported_abis = profile["abis"]
    require(isinstance(abis, list) and 1 <= len(abis) <= len(supported_abis)
            and all(candidate in supported_abis for candidate in abis) and abi in abis and abis == sorted(set(abis)),
            "Invalid Android preflight packaged ABI list")


def check_android_target(receipt: dict, expected: dict, profile: dict, runtime_version: str) -> str:
    target = fields(receipt.get("qualification"), {"host", "connection", "runtime", "physicalXrQualification", "profile"},
                    "Android preflight target")
    connection = expected["adbConnection"]
    require(target["connection"] == {"requested": connection, "observed": connection},
            "Android preflight uses a different ADB connection")
    require(target["profile"] == expected["profile"], "Android preflight uses a different target profile")
    require(target["physicalXrQualification"] == "not_run", "Expected cold discovery evidence")
    host = fields(target["host"], {"requested", "observed"}, "Android preflight host")
    require(in_set(host["requested"], DEPLOY_HOSTS) and host["requested"] == host["observed"],
            "Android preflight did not confirm its selected host")
    runtime = receipt_fields(target["runtime"], {"requested", "selection", "status"}, {"expectedVersion"}, "Android preflight runtime")
    require((runtime.get("requested"), runtime.get("selection"), runtime.get("status")) ==
            ("openxr", "system-default", "cold_system_discovered"), "Expected completed Android OpenXR discovery")
    if "expectedVersion" in runtime:
        require(android_runtime_version(runtime["expectedVersion"]) == runtime_version,
                "Android preflight expected a different runtime version")
    device = receipt_fields(receipt.get("device"), {"buildSha256", "abi"}, {"androidApi", "expectedBuildSha256"}, "Android preflight device")
    require(device["buildSha256"] == expected["deviceBuildSha256"],
            "Android preflight uses a different system build")
    require(device.get("expectedBuildSha256", expected["deviceBuildSha256"]) == expected["deviceBuildSha256"],
            "Android preflight expected a different system build")
    minimum_api = profile["minimumApi"]
    api = device.get("androidApi", minimum_api)
    require(is_json_integer(api) and 0 < api <= 65535, "Invalid Android preflight API level")
    require(api >= minimum_api, "Android preflight API level is below the diagnostic profile minimum")
    require(device["abi"] in profile["abis"], "Android preflight ABI is not supported by the diagnostic profile")
    return device["abi"]


def check_android_discovery(value: object, expected_version: str, abi: str) -> None:
    observed = fields(value, {"crashObserved", "openxr"}, "Android preflight observations")
    require(observed["crashObserved"] is False, "Android preflight recorded a crash")
    openxr = receipt_fields(observed["openxr"], {"abi", "stage", "result", "runtimeVersion"},
                            {"primaryStereoRecommendedViewExtents"}, "Android OpenXR discovery")
    require(openxr["stage"] == "discovered" and is_json_integer(openxr["result"]) and openxr["result"] == 0,
            "Expected successful Android OpenXR discovery")
    require(openxr["abi"] == abi, "Android preflight uses a different runtime ABI")
    require(android_runtime_version(openxr["runtimeVersion"]) == expected_version,
            "Android preflight uses a different runtime version")
    if "primaryStereoRecommendedViewExtents" in openxr:
        check_recommended_stereo_extents(openxr["primaryStereoRecommendedViewExtents"])


def check_recommended_stereo_extents(value: object) -> None:
    """Check cold view recommendations without using them as physical display evidence."""
    require(isinstance(value, list) and len(value) == 2, "Invalid Android stereo view count")
    for entry in value:
        extent = fields(entry, {"width", "height"}, "Android stereo view extent")
        require(all(is_json_integer(dimension) and 0 < dimension < 2**32 for dimension in extent.values()),
                "Invalid Android stereo view extent")


def check_android_preflight(data: bytes, expected: dict, revision: str, runtime_version: str) -> None:
    """Bind a retired Android probe to the independently declared package, device build and runtime."""
    require(0 < len(data) <= MAX_ANDROID_RECEIPT_BYTES, "Android preflight exceeds the size limit")
    require(hashlib.sha256(data).hexdigest() == expected["receiptSha256"], "Android preflight receipt digest does not match")
    receipt = decode_record(data)
    require(isinstance(receipt, dict) and is_json_integer(receipt.get("schema")) and receipt["schema"] == 1,
            "Unsupported Android preflight receipt schema")
    require(receipt.get("status") == "diagnostic_complete" and receipt.get("stage") == "diagnostics",
            "Expected a completed Android preflight")
    require("error" not in receipt and "bundleError" not in receipt, "Android preflight reported a failure")
    fields(receipt, ANDROID_RECEIPT_FIELDS, "Android preflight receipt")
    cleanup = fields(receipt.get("cleanup"), {"status"}, "Android preflight cleanup")
    require(in_set(cleanup["status"], {"stopped", "already_exited"}), "Android preflight process retirement is unconfirmed")
    _, profile = android_profile(expected["profile"])
    abi = check_android_target(receipt, expected, profile, runtime_version)
    check_android_artifact(receipt.get("artifact"), expected, profile, revision, abi)
    check_android_discovery(receipt.get("observed"), runtime_version, abi)


def check_android_preflight_file(path: Path, expected: dict, record: dict) -> None:
    """Match a deployment receipt to the standalone Android build recorded in the physical smoke run."""
    identity = record["tuple"]
    require(is_standalone_android(identity), "Android preflight requires a standalone Android tuple")
    provenance = record["provenance"]
    observed_build = provenance.get("androidDeviceBuildSha256")
    require(observed_build is not None, "Physical smoke record does not identify its Android system build")
    require(observed_build == expected["deviceBuildSha256"],
            "Android preflight and physical smoke use different system builds")
    revision = provenance.get("sourceRevision")
    require(revision is not None, "Device evidence does not record a source revision")
    with path.open("rb") as source:
        check_android_preflight(source.read(MAX_ANDROID_RECEIPT_BYTES + 1), expected, revision, identity["runtimeVersion"])
