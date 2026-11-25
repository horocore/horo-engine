"""Validate bounded Android cold-discovery receipts referenced by an XR release row."""

from __future__ import annotations

import hashlib

from qualify_xr_device import decode_record, fields, in_set, is_digest, require


MAX_ANDROID_RECEIPT_BYTES = 16 * 1024
ANDROID_PROFILES = {"qualification-openxr-development", "qualification-openxr-quest-development"}
ANDROID_ABIS = {"arm64-v8a", "x86_64"}
DEPLOY_HOSTS = {"linux-x64", "macos-arm64", "macos-x64", "windows-x64"}
ANDROID_PREFLIGHT_FIELDS = {"receiptSha256", "apkSha256", "deviceBuildSha256", "profile", "adbConnection"}
OPENXR_ROUTE = {"schema": 1, "runtime": "openxr", "loaderSource": "bundled", "activityRoute": "cold-discovery"}


def is_json_integer(value: object) -> bool:
    """Exclude JSON booleans from integer fields."""
    return isinstance(value, int) and not isinstance(value, bool)


def validate_android_preflight(value: object) -> dict:
    """Read an independently selected diagnostic package and Android target."""
    expected = fields(value, ANDROID_PREFLIGHT_FIELDS, "Android preflight declaration")
    for name in ("receiptSha256", "apkSha256", "deviceBuildSha256"):
        require(is_digest(expected[name]), f"Invalid Android preflight {name}")
    require(in_set(expected["profile"], ANDROID_PROFILES), "Unsupported Android preflight profile")
    require(in_set(expected["adbConnection"], {"usb", "tcpip"}), "Android preflight requires a physical ADB connection")
    return expected


def check_android_artifact(artifact: object, expected: dict, revision: str, abi: str) -> None:
    require(isinstance(artifact, dict), "Missing Android preflight artifact")
    require(artifact.get("apkSha256") == expected["apkSha256"], "Android preflight uses a different diagnostic APK")
    require(artifact.get("sourceRevision") == revision, "Android preflight is from another source revision")
    require(artifact.get("profile") == expected["profile"], "Android preflight uses a different diagnostic profile")
    route = artifact.get("openxr")
    require(route == OPENXR_ROUTE and is_json_integer(route["schema"]) and is_digest(artifact.get("provenanceSha256")),
            "Invalid Android preflight artifact provenance")
    images = fields(artifact.get("openxrImages"), {"abi", "diagnosticHostSha256", "loaderSha256"}, "Android preflight images")
    require(images["abi"] == abi, "Android preflight uses a different packaged ABI")
    require(is_digest(images["diagnosticHostSha256"]) and is_digest(images["loaderSha256"]),
            "Invalid Android preflight image digests")


def check_android_target(receipt: dict, expected: dict) -> str:
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
    runtime = target["runtime"]
    require(isinstance(runtime, dict), "Missing Android preflight runtime")
    require((runtime.get("requested"), runtime.get("selection"), runtime.get("status")) ==
            ("openxr", "system-default", "cold_system_discovered"), "Expected completed Android OpenXR discovery")
    device = receipt.get("device")
    require(isinstance(device, dict) and device.get("buildSha256") == expected["deviceBuildSha256"],
            "Android preflight uses a different system build")
    require(in_set(device.get("abi"), ANDROID_ABIS), "Unsupported Android preflight ABI")
    return device["abi"]


def check_android_discovery(value: object, expected_version: str, abi: str) -> None:
    observed = fields(value, {"crashObserved", "openxr"}, "Android preflight observations")
    require(observed["crashObserved"] is False, "Android preflight recorded a crash")
    openxr = fields(observed["openxr"], {"abi", "stage", "result", "runtimeVersion"}, "Android OpenXR discovery")
    require(openxr["stage"] == "discovered" and is_json_integer(openxr["result"]) and openxr["result"] == 0,
            "Expected successful Android OpenXR discovery")
    require(openxr["abi"] == abi, "Android preflight uses a different runtime ABI")
    version = fields(openxr["runtimeVersion"], {"major", "minor", "patch"}, "Android OpenXR runtime version")
    parts = (version["major"], version["minor"], version["patch"])
    require(all(is_json_integer(part) and low <= part < high for part, low, high in
                zip(parts, (1, 0, 0), (2**16, 2**16, 2**32))), "Invalid Android OpenXR runtime version")
    version_string = ".".join(map(str, parts))
    require(version_string == expected_version, "Android preflight uses a different runtime version")


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
    cleanup = fields(receipt.get("cleanup"), {"status"}, "Android preflight cleanup")
    require(in_set(cleanup["status"], {"stopped", "already_exited"}), "Android preflight process retirement is unconfirmed")
    abi = check_android_target(receipt, expected)
    check_android_artifact(receipt.get("artifact"), expected, revision, abi)
    check_android_discovery(receipt.get("observed"), runtime_version, abi)
