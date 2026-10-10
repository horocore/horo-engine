"""Validated portable Android toolchain/profile inputs and bounded process execution."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import signal
# Explicit host tool capabilities execute argv with shell=False.
import subprocess  # nosec B404

ROOT = Path(__file__).resolve().parents[1]
MAXIMUM_FILE = 256 * 1024 * 1024


# D203 and D211 conflict; retain the PEP-257 no-blank class docstring style.
class AndroidError(ValueError):  # noqa: D203
    """Actionable Android preflight or assembly failure."""


def bounded_bytes(path: Path, maximum: int = MAXIMUM_FILE) -> bytes:
    """Reject links and oversized source artifacts before consuming their bytes."""
    if path.is_symlink() or not path.is_file() or path.stat().st_size > maximum:
        raise AndroidError(f"Invalid or oversized input {path.name}; provide a regular bounded file.")
    with path.open("rb") as source:
        data = source.read(maximum + 1)
    if len(data) > maximum:
        raise AndroidError(f"Input {path.name} changed size; retry with stable files.")
    return data


def digest(path: Path) -> str:
    return hashlib.sha256(bounded_bytes(path)).hexdigest()


def relative_name(value: str) -> str:
    """Admit only portable archive names, including Unicode, without native path normalization."""
    if not isinstance(value, str) or not value or len(value) > 4096:
        raise AndroidError("A nonempty bounded relative input name is required.")
    if any(character in value for character in "\\:\0"):
        raise AndroidError("Archive inputs cannot contain URI/native path syntax.")
    parts = value.split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise AndroidError("Archive input escapes its admitted root; use a relative name.")
    return PurePosixPath(value).as_posix()


def input_path(root: Path, name: str) -> Path:
    result = root / relative_name(name)
    prefix = root
    for part in Path(name).parts:
        prefix /= part
        if prefix.is_symlink():
            raise AndroidError("Input links are not admitted; use ordinary files within the input root.")
    if not result.resolve().is_relative_to(root.resolve()):
        raise AndroidError("Input escapes the admitted root.")
    return result


def load_contract(profile_name: str, abis: list[str]) -> tuple[dict, dict]:
    tools = json.loads(bounded_bytes(ROOT / "android/toolchain-lock.json", 65536))
    profiles = json.loads(bounded_bytes(ROOT / "android/package-profiles.json", 65536))
    if tools.get("schema") != 1 or profiles.get("schema") != 1:
        raise AndroidError("Unsupported Android contract schema; migrate the declared profile.")
    profile = profiles["profiles"].get(profile_name)
    if profile is None or not abis or len(set(abis)) != len(abis):
        raise AndroidError("Select a declared profile and a unique nonempty ABI set.")
    validate_profile(profile, tools, abis)
    return tools, profile


def validate_profile(profile: dict, tools: dict, abis: list[str]) -> None:
    if any(abi not in profile["abis"] or abi not in tools["abis"] for abi in abis):
        raise AndroidError("Unsupported profile/ABI pair; use arm64-v8a or the declared x86_64 development profile.")
    if not tools["minimumApi"] <= profile["minimumApi"] <= profile["targetApi"] <= tools["sdkApi"]:
        raise AndroidError("Incompatible min/target/compile API levels; select the declared API 29/36 tuple.")
    validate_profile_identity(profile)
    validate_asset_names(profile)
    for target, source in profile["assets"].items():
        relative_name(target)
        bounded_bytes(input_path(ROOT, source))


def validate_asset_names(profile: dict) -> None:
    generated = profile["generatedAssets"]
    expected = ["dexopt/baseline.prof", "dexopt/baseline.profm"] if profile["configuration"] == "Release" else []
    if generated != expected:
        raise AndroidError("Generated assets must match the declared Gradle baseline-profile producer contract.")
    reserved = set(generated) | {"horo-package-provenance.json", "horo-java-dependencies.json"}
    if set(profile["assets"]) & reserved:
        raise AndroidError("Source assets cannot overwrite owned provenance or generated package metadata.")


def validate_profile_identity(profile: dict) -> None:
    if profile["renderer"] != "none" or profile["distributionQualified"]:
        raise AndroidError("This qualification composition has no presentation/device support; provide a qualified product host.")
    if profile["permissions"] != [profile["applicationId"] + ".DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION"] or profile["features"]:
        raise AndroidError("Qualification profiles admit only their AndroidX signature-scoped receiver permission and no interactive features.")
    if not re.fullmatch(r"[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+", profile["applicationId"]):
        raise AndroidError("Invalid Android application identity.")
    if not re.fullmatch(r"[a-z][a-z0-9-]*", profile["nativeLibrary"]):
        raise AndroidError("Invalid native library identity.")


def run(command: list[str], directory: Path, log: Path, timeout: int = 1800) -> None:
    """Execute an argv command with an explicit deadline and disk log; never interpolate shell commands."""
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("wb") as output:
        # Caller supplies an explicit host tool capability and argv; no shell.
        # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit
        process = subprocess.Popen(  # nosec B603
            command, cwd=directory, stdout=output, stderr=subprocess.STDOUT, start_new_session=True, shell=False)
        try:
            return_code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            raise AndroidError(f"Android command timed out; inspect {log.name} and retry explicitly.") from error
    if return_code:
        raise AndroidError(f"Android command failed ({return_code}); inspect {log.name}.")


def version_output(command: list[str]) -> str:
    # Fixed version-query argv and explicit tool capability; no shell.
    # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit
    result = subprocess.run(  # nosec B603
        command, check=False, capture_output=True, text=True, timeout=30, shell=False)
    text = result.stdout + result.stderr
    if result.returncode or len(text) > 65536:
        raise AndroidError(f"Cannot identify required tool {Path(command[0]).name}; install its pinned version.")
    return text


def canonical_json(value: dict) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")
