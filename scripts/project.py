"""Portable project authoring, Android packaging and deterministic code generation."""

from __future__ import annotations

from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import dataclass
from functools import cmp_to_key
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import io
import json
import math
import os
import pathlib
import platform as host_platform
import re
from queue import Empty, Queue
import secrets
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
from threading import Thread
import time
import urllib.request
import zipfile


# Android

ROOT = Path(__file__).resolve().parents[1]
MAXIMUM_FILE = 256 * 1024 * 1024
OPENXR_DEVELOPMENT_PROFILE = "qualification-openxr-development"
OPENXR_LOADER_NAME = "libopenxr_loader.so"
OPENXR_LOADER_LICENSE = "licenses/openxr-loader.txt"
OPENXR_PERMISSIONS = ("org.khronos.openxr.permission.OPENXR", "org.khronos.openxr.permission.OPENXR_SYSTEM")
DEPLOY_LAUNCH_EXTRA = "org.horocore.qualification.LAUNCH"
DEPLOY_DEADLINE_EXTRA = "org.horocore.qualification.DEADLINE_MS"
DEPLOY_DEADLINE_MARKER = "host-launch-deadline nonce="
DEPLOY_MAX_SECONDS = 300
DEPLOY_OBSERVATION_MAX_SECONDS = 120
DEPLOY_LOG_TAIL_LINES = 200
DEPLOY_TRANSITION_LIMIT = 512
DEPLOY_TRANSITION_BYTES = 10 * 1024
DEPLOY_LAUNCH_MARKER = "host-launch nonce="
DEPLOY_LAUNCH_CRASH_MARKER = "host-launch-crash nonce="
DEPLOY_ENTRY_MARKER = "host-entry input=accepted"


# D203 and D211 conflict; retain the PEP-257 no-blank class docstring style.
class AndroidError(ValueError):  # noqa: D203
    """Actionable Android preflight or assembly failure."""


class AndroidCommandFailed(AndroidError):  # noqa: D203
    """An admitted command ran to completion and reported failure."""


def bounded_bytes(path: Path, maximum: int = MAXIMUM_FILE) -> bytes:
    """Reject links and oversized source artifacts before consuming their bytes."""
    if path.is_symlink() or not path.is_file() or path.stat().st_size > maximum:
        raise AndroidError(f"Invalid or oversized input {path.name}; provide a regular bounded file.")
    # S8707: explicit CLI input capability; callers admit roots or verify the locked AAR hash.
    with path.open("rb") as source:  # NOSONAR
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


def android_profile(profile_name: str) -> tuple[dict, dict]:
    tools = json.loads(bounded_bytes(ROOT / "android/toolchain-lock.json", 65536))
    profiles = json.loads(bounded_bytes(ROOT / "android/package-profiles.json", 65536))
    if tools.get("schema") != 1 or profiles.get("schema") != 1:
        raise AndroidError("Unsupported Android contract schema; migrate the declared profile.")
    profile = profiles["profiles"].get(profile_name)
    if profile is None:
        raise AndroidError("Select a declared Android profile.")
    if profile_name == OPENXR_DEVELOPMENT_PROFILE:
        loader = json.loads(bounded_bytes(ROOT / "android/openxr-loader-lock.json", 65536))
        if loader.get("schema") != 1:
            raise AndroidError("Unsupported Android OpenXR loader lock schema.")
        tools["openxrLoader"] = loader
    return tools, profile


def load_contract(profile_name: str, abis: list[str]) -> tuple[dict, dict]:
    tools, profile = android_profile(profile_name)
    if not abis or len(set(abis)) != len(abis):
        raise AndroidError("Select a declared profile and a unique nonempty ABI set.")
    validate_profile(profile, tools, abis)
    return tools, profile


def validate_profile(profile: dict, tools: dict, abis: list[str]) -> None:
    if any(abi not in profile["abis"] or abi not in tools["abis"] for abi in abis):
        raise AndroidError("Unsupported profile/ABI pair; use arm64-v8a or the declared x86_64 development profile.")
    if not tools["minimumApi"] <= profile["minimumApi"] <= profile["targetApi"] <= tools["sdkApi"]:
        raise AndroidError("Incompatible min/target/compile API levels; select the declared API 29/36 tuple.")
    validate_profile_identity(profile)
    validate_openxr_profile(profile, tools, abis)
    validate_asset_names(profile)
    for target, source in profile["assets"].items():
        relative_name(target)
        bounded_bytes(input_path(ROOT, source))


def validate_asset_names(profile: dict) -> None:
    generated = profile["generatedAssets"]
    expected = ["dexopt/baseline.prof", "dexopt/baseline.profm"] if profile["configuration"] == "Release" else []
    if generated != expected:
        raise AndroidError("Generated assets must match the declared Gradle baseline-profile producer contract.")
    reserved = set(generated) | {"horo-package-provenance.json", JAVA_DEPENDENCIES_ASSET}
    if "openxrProfile" in profile:
        reserved.add(OPENXR_LOADER_LICENSE)
    if set(profile["assets"]) & reserved:
        raise AndroidError("Source assets cannot overwrite owned provenance or generated package metadata.")


def exact_profile_fields(actual: object, expected: dict) -> bool:
    """Require the closed profile's exact field types as well as values."""
    if not isinstance(actual, dict) or actual.keys() != expected.keys():
        return False
    return all(type(actual[key]) is type(value) and actual[key] == value for key, value in expected.items())


def validate_renderer_profile(profile: dict) -> None:
    """Admit only explicit assembly-only or bounded Android Vulkan development composition."""
    if profile["distributionQualified"]:
        raise AndroidError("Qualification packages cannot claim distribution/device qualification.")
    if profile["renderer"] == "none":
        if "graphicsProfile" in profile:
            raise AndroidError("Assembly-only profiles cannot declare native graphics support.")
        return
    expected = {"schema": 2, "api": "Vulkan1.1", "presentMode": "Fifo",
                "swapchainMaintenance1Required": True, "singleGraphicsPresentQueue": True,
                "compositeAlphaPolicy": "prefer-opaque-else-inherit-alpha-one"}
    if profile["renderer"] != "vulkan-android" or not exact_profile_fields(profile.get("graphicsProfile"), expected):
        raise AndroidError("Unsupported Android development graphics profile; select the declared effective Vulkan tuple.")
    if profile["preset"] != "android-vulkan-development" or profile["configuration"] != "Debug":
        raise AndroidError("Android Vulkan qualification requires its explicit development preset.")
    if profile["assets"] != {"qualification.json": "android/qualification-vulkan.json"}:
        raise AndroidError("Android Vulkan development requires its presentation-intent asset without device qualification claims.")


def validate_profile_identity(profile: dict) -> None:
    validate_renderer_profile(profile)
    permissions = [profile["applicationId"] + ".DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION", "android.permission.ACTIVITY_RECOGNITION"]
    if "openxrProfile" in profile:
        permissions.extend(OPENXR_PERMISSIONS)
    if profile["permissions"] != permissions or profile["features"]:
        raise AndroidError("Qualification profile permissions differ from its declared capability policy or require hardware features.")
    if not re.fullmatch(r"[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+", profile["applicationId"]):
        raise AndroidError("Invalid Android application identity.")
    if not re.fullmatch(r"[a-z][a-z0-9-]*", profile["nativeLibrary"]):
        raise AndroidError("Invalid native library identity.")


def validate_openxr_profile(profile: dict, tools: dict, abis: list[str]) -> None:
    lock = tools.get("openxrLoader")
    if lock is None:
        if "openxrProfile" in profile or OPENXR_LOADER_NAME in profile["nativeLibraries"]:
            raise AndroidError("Only the declared OpenXR profile may include an OpenXR loader.")
        return
    expected = {"schema": 1, "runtime": "openxr", "loaderSource": "bundled", "activityRoute": "cold-discovery"}
    if (not exact_profile_fields(profile.get("openxrProfile"), expected) or profile["preset"] != "android-openxr-development"
            or profile["configuration"] != "Debug" or profile["renderer"] != "none"):
        raise AndroidError("The Android OpenXR profile requires its explicit cold-discovery preset.")
    if profile["nativeLibraries"].count(OPENXR_LOADER_NAME) != 1 or any(abi not in lock["abiSha256"] for abi in abis):
        raise AndroidError("OpenXR loader packaging requires one pinned image for every selected ABI.")


def admitted_command(command: list[str]) -> list[str]:
    """Require separate literal arguments, never an implicitly parsed command line."""
    if not isinstance(command, list) or not command or any(
            not isinstance(argument, str) or "\0" in argument for argument in command):
        raise AndroidError("A nonempty argv list of NUL-free string arguments is required.")
    executable = command[0]
    if not executable:
        raise AndroidError("The executable capability cannot be empty.")
    if "/" in executable or "\\" in executable:
        # Explicit relative paths are capabilities too; anchor them before cwd changes.
        executable = str(Path(executable).absolute())
    if not Path(executable).is_absolute():
        # PATH is the caller's host-tool capability, not the child project cwd.
        # Ignore implicit/relative search roots, including Windows' default CWD search.
        roots = [Path(root) for root in os.get_exec_path() if root and Path(root).is_absolute()]
        executable = next((found for root in roots
                           if (found := shutil.which(str(root / executable))) is not None), "")
    path = Path(executable)
    if not executable or not path.is_file() or not os.access(path, os.X_OK):
        raise AndroidError("Host executable unavailable; install the tool or supply its absolute path.")
    # Preserve venv/symlink invocation identity after validating the real file.
    executable = str(path.absolute())
    if os.name == "nt" and Path(executable).suffix.lower() in {".bat", ".cmd"}:
        raise AndroidError("Windows batch tools invoke an implicit shell; select a native executable instead.")
    return [executable, *command[1:]]


def execution_environment() -> dict[str, str]:
    """Retain SDK/signing configuration while removing selected startup hooks.

    Selected executables, toolchain library paths and JVM options remain trusted
    caller capabilities. This is not a comprehensive environment sanitizer or a
    sandbox for an untrusted compiler or Gradle distribution.
    """
    startup_hooks = {"LD_PRELOAD", "LD_AUDIT", "DYLD_INSERT_LIBRARIES", "PYTHONPATH",
                     "PYTHONHOME", "PYTHONSTARTUP", "PYTHONINSPECT", "BASH_ENV", "ENV"}
    environment = {key: value for key, value in os.environ.items() if key.upper() not in startup_hooks}
    if os.name == "nt":
        environment["NoDefaultCurrentDirectoryInExePath"] = "1"
    return environment


def android_run(command: list[str], directory: Path, log: Path, timeout: int = 1800) -> None:
    """Execute an argv command with an explicit deadline and disk log; never interpolate shell commands."""
    command = admitted_command(command)
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("wb") as output:
        process = subprocess.Popen(
            command, cwd=directory, stdout=output, stderr=subprocess.STDOUT,
            start_new_session=True, shell=False, env=execution_environment())
        try:
            return_code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            if os.name == "nt":
                process.kill()
            else:
                os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            raise AndroidError(f"Android command timed out; inspect {log.name} and retry explicitly.") from error
    if return_code:
        raise AndroidCommandFailed(f"Android command failed ({return_code}); inspect {log.name}.")


def run_gradle_task(gradle: Path, task: str, stage: Path, log: Path) -> None:
    """Retry Gradle repository throttling while preserving each attempt's log."""
    command = [str(gradle.resolve()), "--no-daemon", "--console=plain", task]
    delays = (20, 60)
    for attempt, delay in enumerate(delays, start=1):
        try:
            android_run(command, stage, log)
            return
        except AndroidCommandFailed:
            with log.open("rb") as output:
                output.seek(max(0, log.stat().st_size - 65536))
                throttled = b"Received status code 429 from server: Too Many Requests" in output.read(65536)
            if not throttled:
                raise
            log.replace(log.with_name(f"{log.stem}-attempt-{attempt}{log.suffix}"))
            print(f"Android dependency repository rate limited the request. Retrying in {delay} seconds.", file=sys.stderr)
            time.sleep(delay)
    android_run(command, stage, log)


def version_output(command: list[str]) -> str:
    command = admitted_command(command)
    result = subprocess.run(
        command, check=False, capture_output=True, text=True, timeout=30,
        shell=False, env=execution_environment())
    text = result.stdout + result.stderr
    if result.returncode or len(text) > 65536:
        raise AndroidError(f"Cannot identify required tool {Path(command[0]).name}; install its pinned version.")
    return text


def canonical_json(value: dict) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")


# Elf

def unpack(data: bytes, format_string: str, offset: int) -> tuple:
    size = struct.calcsize(format_string)
    if offset < 0 or offset + size > len(data):
        raise AndroidError("Truncated ELF metadata; rebuild the native dependency.")
    return struct.unpack_from(format_string, data, offset)


def programs(data: bytes) -> list[tuple]:
    header = unpack(data, "<HHIQQQIHHHHHH", 16)
    offset, entry_size, count = header[4], header[8], header[9]
    if entry_size != 56 or count > 1024:
        raise AndroidError("Unsupported ELF program table; rebuild with the declared NDK.")
    return [unpack(data, "<IIQQQQQQ", offset + index * entry_size) for index in range(count)]


def dynamic_entries(data: bytes, headers: list[tuple]) -> list[tuple]:
    dynamic = next((header for header in headers if header[0] == 2), None)
    if dynamic is None or dynamic[5] > 65536 or dynamic[5] % 16:
        raise AndroidError("Missing or invalid ELF dynamic metadata.")
    result = []
    for offset in range(dynamic[2], dynamic[2] + dynamic[5], 16):
        entry = unpack(data, "<qQ", offset)
        if entry[0] == 0:
            return result
        result.append(entry)
    raise AndroidError("Unterminated ELF dynamic metadata.")


def string_table(data: bytes, headers: list[tuple], entries: list[tuple]) -> bytes:
    strings = next((value for tag, value in entries if tag == 5), None)
    size = next((value for tag, value in entries if tag == 10), 0)
    if strings is None or size > 1024 * 1024:
        raise AndroidError("Invalid ELF string table.")
    for segment in headers:
        if segment[0] != 1:
            continue
        if segment[3] <= strings < segment[3] + segment[5]:
            return segment_strings(data, segment, strings, size)
    raise AndroidError("ELF string table has no mapped load segment.")


def segment_strings(data: bytes, segment: tuple, strings: int, size: int) -> bytes:
    offset = segment[2] + strings - segment[3]
    if offset + size > len(data) or strings + size > segment[3] + segment[5]:
        raise AndroidError("Truncated ELF string table.")
    return data[offset:offset + size]


def needed_names(data: bytes, headers: list[tuple]) -> list[str]:
    entries = dynamic_entries(data, headers)
    if any(tag in (15, 29) for tag, _ in entries):
        raise AndroidError("Native RPATH/RUNPATH is forbidden; package an explicit Android dependency closure.")
    table = string_table(data, headers, entries)
    return [library_name(table, value) for tag, value in entries if tag == 1]


def android_api(data: bytes, headers: list[tuple]) -> int:
    for header in headers:
        if header[0] == 4:
            api = note_api(data, header[2], header[5])
            if api is not None:
                return api
    raise AndroidError("Native dependency has no Android API provenance note; rebuild with the admitted NDK.")


def note_api(data: bytes, offset: int, size: int) -> int | None:
    end = offset + size
    if end > len(data):
        raise AndroidError("Truncated ELF note metadata.")
    while offset + 12 <= end:
        name_size, description_size, kind = unpack(data, "<III", offset)
        name_offset = offset + 12
        description_offset = name_offset + ((name_size + 3) & ~3)
        next_offset = description_offset + ((description_size + 3) & ~3)
        if next_offset > end:
            raise AndroidError("Truncated ELF note payload.")
        if kind == 1 and data[name_offset:name_offset + name_size].rstrip(b"\0") == b"Android":
            if description_size < 4:
                raise AndroidError("Invalid Android API note.")
            return unpack(data, "<I", description_offset)[0]
        offset = next_offset
    return None


def library_name(table: bytes, offset: int) -> str:
    end = table.find(b"\0", offset)
    if offset >= len(table) or end < 0:
        raise AndroidError("Invalid native dependency name.")
    try:
        name = table[offset:end].decode("ascii")
    except UnicodeDecodeError as error:
        raise AndroidError("Nonportable native dependency name.") from error
    if not name.startswith("lib") or not name.endswith(".so") or "/" in name or "\\" in name:
        raise AndroidError("Native dependency names must be ordinary library basenames.")
    return name


def validate_load_alignment(headers: list[tuple]) -> None:
    loads = [header for header in headers if header[0] == 1]
    if not loads or any(header[7] < 16384 for header in loads):
        raise AndroidError("Native dependency lacks 16 KiB load alignment; rebuild with the pinned Android linker policy.")


def inspect_elf(path: Path, machine: int, minimum_api: int = 29) -> dict:
    data = bounded_bytes(path)
    if len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01":
        raise AndroidError("Native input is not a little-endian ELF64 artifact.")
    elf_type, actual_machine = unpack(data, "<HH", 16)
    if elf_type != 3 or actual_machine != machine:
        raise AndroidError("Native dependency ABI is incompatible; rebuild each declared ABI independently.")
    headers = programs(data)
    validate_load_alignment(headers)
    api = android_api(data, headers)
    if not 21 <= api <= minimum_api:
        raise AndroidError("Native dependency requires a newer API than minSdk; rebuild for the profile API floor.")
    return {"name": path.name, "sha256": hashlib.sha256(data).hexdigest(), "size": len(data),
            "minimumApi": api, "needed": sorted(needed_names(data, headers))}


def native_closure(directory: Path, abi: str, tools: dict, entry_library: str) -> list[dict]:
    if directory.is_symlink():
        raise AndroidError("Native ABI input directories cannot be links.")
    libraries = sorted(directory.glob("*.so"))
    if not libraries or len(libraries) > 256:
        raise AndroidError("Missing or oversized native library set; provide the full declared ABI closure.")
    names = {path.name for path in libraries}
    if f"lib{entry_library}.so" not in names or "libc++_shared.so" not in names:
        raise AndroidError("Package needs its entry library and exactly one pinned shared C++ runtime per ABI.")
    records = [inspect_elf(path, tools["abis"][abi], tools["minimumApi"]) for path in libraries]
    validate_dependency_closure(records, names | set(tools["systemLibraries"]))
    return records


def validate_dependency_closure(records: list[dict], allowed: set[str]) -> None:
    missing = sorted({name for record in records for name in record["needed"] if name not in allowed})
    if missing:
        raise AndroidError(f"Missing native dependencies: {', '.join(missing)}; supply the complete ABI closure.")


# Android Tools

def require_revision(root: Path, expected: str, label: str) -> None:
    if not (root / "source.properties").is_file():
        raise AndroidError(f"{label} unavailable; install its pinned {expected} SDK component and pass its root explicitly.")
    properties = bounded_bytes(root / "source.properties", 65536).decode("utf-8")
    match = re.search(r"^Pkg.Revision\s*=\s*(\S+)\s*$", properties, re.MULTILINE)
    if match is None or match[1] != expected:
        raise AndroidError(f"{label} version drift; install the pinned {expected} SDK component.")


def signing_inputs() -> dict:
    required = ("HORO_ANDROID_KEYSTORE", "HORO_ANDROID_KEY_ALIAS", "HORO_ANDROID_STORE_PASSWORD", "HORO_ANDROID_KEY_PASSWORD")
    if any(not os.environ.get(name) for name in required):
        raise AndroidError("Signing profile unavailable; supply HORO_ANDROID_KEYSTORE/KEY_ALIAS/STORE_PASSWORD/KEY_PASSWORD externally.")
    keystore = Path(os.environ["HORO_ANDROID_KEYSTORE"])
    bounded_bytes(keystore, 1024 * 1024)
    try:
        version_output(["keytool", "-list", "-keystore", str(keystore), "-storepass:env", "HORO_ANDROID_STORE_PASSWORD",
                        "-alias", os.environ["HORO_ANDROID_KEY_ALIAS"]])
    except AndroidError as error:
        raise AndroidError("Signing identity cannot be opened; check the external keystore, password and alias.") from error
    return {"keystore": str(keystore), "alias": os.environ["HORO_ANDROID_KEY_ALIAS"]}


def tools_preflight(tools: dict, sdk: Path, ndk: Path, gradle: Path, signed: bool) -> dict:
    if sys.platform != "linux":
        raise AndroidError("The initial pinned package assembly host is Linux; use its hosted qualification workflow.")
    require_revision(ndk, tools["ndk"], "NDK")
    build_tools = sdk / "build-tools" / tools["buildTools"]
    require_revision(build_tools, tools["buildTools"], "Build tools")
    bounded_bytes(sdk / "platforms" / f"android-{tools['sdkApi']}" / "android.jar")
    require_version(["cmake", "--version"], rf"cmake version {re.escape(tools['cmake'])}(?:\s|$)", "CMake")
    require_version(["ninja", "--version"], rf"^{re.escape(tools['ninja'])}(?:\s|$)", "Ninja")
    require_version([str(gradle), "--version"], rf"Gradle {re.escape(tools['gradle'])}(?:\s|$)", "Gradle")
    require_version(["java", "-version"], rf'version "{tools["jdkMajor"]}(?:\.|\")', "JDK")
    paths = {name: str(build_tools / name) for name in ("apksigner", "aapt2", "zipalign")}
    for path in paths.values():
        if not Path(path).is_file() or not os.access(path, os.X_OK):
            raise AndroidError("Pinned Android build tools are missing; install the declared build-tools SDK package.")
    return {"commands": paths, "signing": signing_inputs() if signed else None}


def require_version(command: list[str], pattern: str, label: str) -> None:
    if re.search(pattern, version_output(command)) is None:
        raise AndroidError(f"{label} version drift; install the exact toolchain-lock.json version.")


def extract_game_activity(archive: Path, target: Path, tools: dict, abis: list[str]) -> None:
    if digest(archive) != tools["gameActivity"]["sha256"]:
        raise AndroidError("GameActivity digest mismatch; obtain the declared pinned AAR from Google Maven.")
    with zipfile.ZipFile(archive) as source:
        entries = source.infolist()
        if len(entries) != len({entry.filename for entry in entries}) or len(entries) > 4096:
            raise AndroidError("GameActivity archive has ambiguous or excessive entries.")
        if sum(entry.file_size for entry in entries) > 64 * 1024 * 1024:
            raise AndroidError("GameActivity archive exceeds the bounded dependency budget.")
        for entry in entries:
            extract_entry(source, entry, target)
    for abi in abis:
        validate_activity_metadata(target, tools, abi)


def validate_activity_metadata(target: Path, tools: dict, abi: str) -> None:
    metadata_path = target / f"prefab/modules/game-activity_static/libs/android.{abi}/abi.json"
    metadata = json.loads(bounded_bytes(metadata_path, 65536))
    if metadata["abi"] != abi or metadata["api"] > tools["minimumApi"] or metadata["stl"] != "c++_shared":
        raise AndroidError("GameActivity API/ABI/STL contract is incompatible; update its admitted lock explicitly.")
    if not metadata["static"] or metadata["ndk"] > int(tools["ndk"].split(".")[0]):
        raise AndroidError("GameActivity native toolchain contract is incompatible.")


def extract_entry(source: zipfile.ZipFile, entry: zipfile.ZipInfo, target: Path) -> None:
    name = relative_name(entry.filename.rstrip("/"))
    if (entry.external_attr >> 16) & 0o170000 == 0o120000:
        raise AndroidError("Archive dependency links are forbidden.")
    if entry.is_dir():
        return
    destination = target / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(source.read(entry))


# Apk

#!/usr/bin/env python3


ASSET_PREFIX = "assets/"
PROVENANCE_ASSET = ASSET_PREFIX + "horo-package-provenance.json"
JAVA_DEPENDENCIES_ASSET = "horo-java-dependencies.json"


def copy_assets(profile: dict, destination: Path) -> list[dict]:
    records = []
    for target, name in sorted(profile["assets"].items()):
        source = input_path(ROOT, name)
        output = destination / target
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(bounded_bytes(source))
        records.append({"name": target, "sha256": digest(output), "size": output.stat().st_size})
    return records


@dataclass(frozen=True)
class AndroidOpenXRLoader:
    images: dict[str, bytes]
    license: bytes


def read_openxr_loader(source: Path | None, tools: dict, abis: list[str], unsigned: bool) -> AndroidOpenXRLoader | None:
    lock = tools.get("openxrLoader")
    if lock is None:
        if source is not None:
            raise AndroidError("The OpenXR loader AAR is only accepted by the explicit OpenXR profile.")
        return None
    if unsigned or source is None:
        raise AndroidError("The OpenXR development profile requires signing and the explicit locked loader AAR.")
    data = bounded_bytes(source, 8 * 1024 * 1024)
    if hashlib.sha256(data).hexdigest() != lock["sha256"]:
        raise AndroidError("Android OpenXR loader AAR digest mismatch.")
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        archive_limits(archive)
        loaders = {abi: archive.read(f"jni/{abi}/{OPENXR_LOADER_NAME}") for abi in abis}
        license_text = archive.read("META-INF/LICENSE")
    for abi, image in loaders.items():
        if hashlib.sha256(image).hexdigest() != lock["abiSha256"][abi]:
            raise AndroidError(f"Android OpenXR loader digest mismatch for {abi}.")
    if hashlib.sha256(license_text).hexdigest() != lock["licenseSha256"]:
        raise AndroidError("Android OpenXR loader license digest mismatch.")
    return AndroidOpenXRLoader(loaders, license_text)


def stage_openxr_loader(destination: Path, image: bytes, tools: dict, abi: str) -> dict:
    path = destination / OPENXR_LOADER_NAME
    path.write_bytes(image)
    record = inspect_elf(path, tools["abis"][abi], tools["minimumApi"])
    if record["sha256"] != tools["openxrLoader"]["abiSha256"][abi]:
        raise AndroidError(f"Staged Android OpenXR loader digest mismatch for {abi}.")
    validate_dependency_closure([record], set(tools["systemLibraries"]))
    return record


def stage_native(native_root: Path, stage: Path, tools: dict, profile: dict, abis: list[str],
                 openxr_loaders: dict[str, bytes] | None = None) -> dict:
    openxr_loaders = openxr_loaders or {}
    expected = set(profile["nativeLibraries"])
    if OPENXR_LOADER_NAME in expected:
        if set(openxr_loaders) != set(abis):
            raise AndroidError("The staged OpenXR loader images must cover the selected ABI set exactly.")
        expected.remove(OPENXR_LOADER_NAME)
    elif openxr_loaders:
        raise AndroidError("The selected Android profile does not permit a loader.")
    result = {}
    for abi in abis:
        source = native_root / abi
        records = native_closure(source, abi, tools, profile["nativeLibrary"])
        if {record["name"] for record in records} != expected:
            raise AndroidError("Native inputs differ from the explicitly declared library set.")
        destination = stage / "native" / abi
        destination.mkdir(parents=True)
        for record in records:
            shutil.copyfile(source / record["name"], destination / record["name"])
            if digest(destination / record["name"]) != record["sha256"]:
                raise AndroidError("Native input changed during staging; retry with stable files.")
        if abi in openxr_loaders:
            records.append(stage_openxr_loader(destination, openxr_loaders[abi], tools, abi))
        result[abi] = sorted(records, key=lambda record: record["name"])
    return result


def apk_manifest(profile: dict) -> str:
    application = profile["applicationId"]  # Validated restricted identifier; cannot contain XML metacharacters.
    library = profile["nativeLibrary"]
    openxr = "" if "openxrProfile" not in profile else """
  <uses-permission android:name="org.khronos.openxr.permission.OPENXR" />
  <uses-permission android:name="org.khronos.openxr.permission.OPENXR_SYSTEM" />
  <queries>
    <provider android:authorities="org.khronos.openxr.runtime_broker;org.khronos.openxr.system_runtime_broker" />
    <intent><action android:name="org.khronos.openxr.OpenXRRuntimeService" /></intent>
    <intent><action android:name="org.khronos.openxr.OpenXRApiLayerService" /></intent>
  </queries>"""
    return f'''<manifest xmlns:android="http://schemas.android.com/apk/res/android">
  <uses-feature android:name="android.hardware.touchscreen" android:required="false" />
  <uses-permission android:name="android.permission.ACTIVITY_RECOGNITION" />{openxr}
  <application android:label="Horo Package Qualification" android:theme="@style/Theme.AppCompat.NoActionBar"
               android:extractNativeLibs="false" android:allowBackup="false">
    <activity android:name="{application}.{PACKAGE_ACTIVITY}" android:exported="true" android:configChanges="orientation|screenSize|keyboardHidden">
      <meta-data android:name="android.app.lib_name" android:value="{library}" />
      <intent-filter><action android:name="android.intent.action.MAIN" /><category android:name="android.intent.category.LAUNCHER" /></intent-filter>
    </activity>
  </application>
</manifest>
'''


def apk_write_project(stage: Path, tools: dict, profile: dict, abis: list[str], activity: Path) -> None:
    for source in (ROOT / "android/gradle").iterdir():
        shutil.copyfile(source, stage / source.name)
    (stage / "contract.json").write_bytes(canonical_json({"tools": tools, "profile": profile, "abis": sorted(abis)}))
    (stage / "AndroidManifest.xml").write_text(apk_manifest(profile), encoding="utf-8")
    java_root = stage / "java" / Path(*profile["applicationId"].split("."))
    java_root.mkdir(parents=True)
    activity_base = "org.horocore.platform.HoroInputActivity"
    crash_observer = ""
    deadline_members = ""
    deadline_intent = ""
    if profile["renderer"] == "vulkan-android":
        package = Path("org/horocore/qualification")
        destination = stage / "java" / package
        destination.mkdir(parents=True, exist_ok=True)
        for source_name in (
            "HoroVulkanQualificationActivity.java", "PowerObservation.java", "DisplayRefreshObservation.java", "MemoryObservation.java"
        ):
            shutil.copyfile(ROOT / "apps/android-package-qualification/java" / package / source_name, destination / source_name)
        activity_base = "org.horocore.qualification.HoroVulkanQualificationActivity"
    if "openxrProfile" in profile:
        observer = Path("org/horocore/qualification/OpenXRLaunchCrashObserver.java")
        destination = stage / "java" / observer
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / "apps/android-package-qualification/java" / observer, destination)
        deadline_members = (
            "    private static long diagnosticExpiresAt;\n"
            "    private final android.os.Handler diagnosticHandler = new android.os.Handler(android.os.Looper.getMainLooper());\n"
            "    private final Runnable endDiagnostic = () -> { if (!isFinishing() && !isDestroyed()) finishAndRemoveTask(); };\n"
            "    private boolean scheduleDiagnosticEnd() {\n"
            "        diagnosticHandler.removeCallbacks(endDiagnostic);\n"
            "        long remaining = Math.max(0L, diagnosticExpiresAt - android.os.SystemClock.elapsedRealtime());\n"
            "        return diagnosticHandler.postDelayed(endDiagnostic, remaining);\n"
            "    }\n"
            "    private boolean armDiagnosticDeadline(int milliseconds) {\n"
            f"        if (milliseconds <= 0 || milliseconds > {DEPLOY_MAX_SECONDS * 1000}) return false;\n"
            "        diagnosticExpiresAt = android.os.SystemClock.elapsedRealtime() + milliseconds;\n"
            "        if (scheduleDiagnosticEnd()) return true;\n"
            "        diagnosticExpiresAt = 0L;\n"
            "        return false;\n"
            "    }\n"
            "    @Override public void onResume() {\n"
            "        super.onResume();\n"
            "        if (diagnosticExpiresAt != 0L) scheduleDiagnosticEnd();\n"
            "    }\n"
            "    @Override public void onDestroy() {\n"
            "        diagnosticHandler.removeCallbacks(endDiagnostic);\n"
            "        super.onDestroy();\n"
            "    }\n")
        deadline_intent = (f"        int deadlineMs = intent.getIntExtra(\"{DEPLOY_DEADLINE_EXTRA}\", 0);\n"
                           f"        intent.removeExtra(\"{DEPLOY_DEADLINE_EXTRA}\");\n")
        crash_observer = (
            "                org.horocore.qualification.OpenXRLaunchCrashObserver.install(nonce, started);\n"
            "                if (armDiagnosticDeadline(deadlineMs)) {\n"
            f"                    android.util.Log.i(\"HoroPackageQualification\", \"{DEPLOY_DEADLINE_MARKER}\"\n"
            "                            + nonce + \" startTicks=\" + started + \" milliseconds=\" + deadlineMs);\n"
            "                }\n")
    source = (f"package {profile['applicationId']};\n"
              f"public final class PackageActivity extends {activity_base} {{\n"
              f"    static {{ System.loadLibrary(\"{profile['nativeLibrary']}\"); }}\n"
              f"{deadline_members}"
              "    private static boolean activityCreated;\n"
              "    private static String processStartTicks() {\n"
              "        try (java.io.BufferedReader reader = new java.io.BufferedReader(new java.io.FileReader(\"/proc/self/stat\"))) {\n"
              "            String stat = reader.readLine();\n"
              "            int end = stat == null ? -1 : stat.lastIndexOf(\") \");\n"
              "            if (end < 0) return null;\n"
              "            String[] fields = stat.substring(end + 2).split(\" +\", 21);\n"
              "            if (fields.length > 19 && fields[19].matches(\"[1-9][0-9]{0,19}\")) return fields[19];\n"
              "        } catch (java.io.IOException ignored) { }\n"
              "        return null;\n"
              "    }\n"
              "    @Override public void onCreate(android.os.Bundle state) {\n"
              "        boolean firstActivity = !activityCreated;\n"
              "        activityCreated = true;\n"
              "        android.content.Intent intent = getIntent();\n"
              f"        String nonce = intent.getStringExtra(\"{DEPLOY_LAUNCH_EXTRA}\");\n"
              f"        intent.removeExtra(\"{DEPLOY_LAUNCH_EXTRA}\");\n"
              f"{deadline_intent}"
              "        super.onCreate(state);\n"
              "        if (firstActivity && state == null && nonce != null && nonce.matches(\"[0-9a-f]{32}\")) {\n"
              "            String started = processStartTicks();\n"
              "            if (started != null) {\n"
              f"                android.util.Log.i(\"HoroPackageQualification\", \"{DEPLOY_LAUNCH_MARKER}\" + nonce + \" startTicks=\" + started);\n"
              f"{crash_observer}"
              "            }\n"
              "        }\n"
              "    }\n}\n")
    (java_root / "PackageActivity.java").write_text(source, encoding="utf-8")
    input_root = stage / "java/org/horocore/platform"
    input_root.mkdir(parents=True, exist_ok=True)
    input_source = ROOT / "apps/android-input/java/org/horocore/platform/HoroInputActivity.java"
    shutil.copyfile(input_source, input_root / "HoroInputActivity.java")
    shutil.copyfile(activity, stage / "game-activity.aar")
    if digest(stage / "game-activity.aar") != tools["gameActivity"]["sha256"]:
        raise AndroidError("GameActivity changed during staging; retry with the admitted AAR.")


def ndk_host_tag() -> str:
    """Select the NDK host package for this build machine without searching foreign installations."""
    tags = {"Linux": "linux-x86_64", "Darwin": "darwin-x86_64", "Windows": "windows-x86_64"}
    tag = tags.get(host_platform.system())
    if tag is None:
        raise AndroidError("Unsupported NDK build host; select a declared Linux, macOS or Windows toolchain.")
    return tag


def ndk_shared_runtime(ndk: Path, abi: str) -> Path:
    """Resolve the exact declared ABI runtime within the selected host's NDK sysroot."""
    triples = {"arm64-v8a": "aarch64-linux-android", "x86_64": "x86_64-linux-android"}
    triple = triples.get(abi)
    if triple is None:
        raise AndroidError("Unsupported Android runtime ABI.")
    runtime = ndk / f"toolchains/llvm/prebuilt/{ndk_host_tag()}/sysroot/usr/lib/{triple}/libc++_shared.so"
    bounded_bytes(runtime)
    return runtime


def build_native(arguments: argparse.Namespace, profile: dict, activity_root: Path, workspace: Path) -> Path:
    if arguments.native_root:
        if arguments.native_root.is_symlink():
            raise AndroidError("Native input roots cannot be links.")
        return arguments.native_root.resolve()
    native_root = workspace / "native-inputs"
    os.environ["ANDROID_NDK_ROOT"] = str(arguments.ndk.resolve())
    os.environ["HORO_GAME_ACTIVITY_ROOT"] = str(activity_root)
    for abi in arguments.abi:
        os.environ["HORO_ANDROID_ABI"] = abi
        binary = workspace / "cmake" / abi
        android_run(["cmake", "--preset", profile["preset"], "-B", str(binary)], ROOT, workspace / f"configure-{abi}.log")
        android_run(["cmake", "--build", str(binary), "--target", "HoroAndroidPackageQualification", "--parallel", "2"],
            ROOT, workspace / f"build-{abi}.log")
        destination = native_root / abi
        destination.mkdir(parents=True)
        shutil.copyfile(binary / f"lib{profile['nativeLibrary']}.so", destination / f"lib{profile['nativeLibrary']}.so")
        runtime = ndk_shared_runtime(arguments.ndk, abi)
        shutil.copyfile(runtime, destination / "libc++_shared.so")
    return native_root


def provenance(tools: dict, profile: dict, abis: dict, assets: list[dict]) -> dict:
    revision = version_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"]).strip()
    return {"schema": 1, "sourceRevision": revision, "toolchain": tools, "profile": profile,
            "abis": abis, "assets": assets, "sourceDateEpoch": 315532800,
            "javaDependencyLockSha256": digest(ROOT / "android/gradle/gradle.lockfile")}


def archive_limits(archive: zipfile.ZipFile) -> None:
    entries = archive.infolist()
    names = [entry.filename for entry in entries]
    if len(names) != len(set(names)) or len(names) > 65536:
        raise AndroidError("Ambiguous or oversized APK contents.")
    if any(entry.file_size > 256 * 1024 * 1024 for entry in entries) or sum(entry.file_size for entry in entries) > 1024 * 1024 * 1024:
        raise AndroidError("APK exceeds the bounded application content budget.")
    for entry in entries:
        relative_name(entry.filename.rstrip("/"))
        if (entry.external_attr >> 16) & 0o170000 == 0o120000:
            raise AndroidError("APK content links are forbidden.")


def canonical_apk(source: Path, destination: Path, provenance_bytes: bytes | None = None) -> None:
    with zipfile.ZipFile(source) as original, zipfile.ZipFile(destination, "w") as result:
        archive_limits(original)
        names = original.namelist()
        for name in sorted(names):
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED if name.endswith(".so") or name == "resources.arsc" else zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            payload = provenance_bytes if name == PROVENANCE_ASSET and provenance_bytes is not None else original.read(name)
            result.writestr(info, payload)


def sign_package(package: Path, signed: Path, tools: dict, capability: dict, workspace: Path) -> None:
    identity = capability["signing"]
    android_run([capability["commands"]["apksigner"], "sign", "--ks", identity["keystore"], "--ks-key-alias", identity["alias"],
         "--ks-pass", "env:HORO_ANDROID_STORE_PASSWORD", "--key-pass", "env:HORO_ANDROID_KEY_PASSWORD",
         "--v1-signing-enabled", "false", "--v2-signing-enabled", "true", "--v3-signing-enabled", "false",
         "--min-sdk-version", str(tools["minimumApi"]), "--out", str(signed), str(package)], workspace, workspace / "sign.log")
    android_run([capability["commands"]["apksigner"], "verify", "--verbose", str(signed)], workspace, workspace / "verify-signature.log")


def inspect_package(package: Path, expected: dict) -> dict:
    with zipfile.ZipFile(package) as archive:
        archive_limits(archive)
        names = archive.namelist()
        if len(names) != len(set(names)) or "classes.dex" not in names or "AndroidManifest.xml" not in names:
            raise AndroidError("APK is ambiguous or missing its Android application payload.")
        if "resources.arsc" in names and archive.getinfo("resources.arsc").compress_type != zipfile.ZIP_STORED:
            raise AndroidError("Android resource tables must be uncompressed before four-byte zip alignment.")
        provenance_bytes = archive.read(PROVENANCE_ASSET)
        if provenance_bytes != canonical_json(expected):
            raise AndroidError("APK provenance differs from its admitted inputs.")
        verify_native_contents(archive, expected)
        verify_assets(archive, expected)
        return {"schema": 1, "apkSha256": digest(package), "provenanceSha256": hashlib.sha256(provenance_bytes).hexdigest(),
                "contents": [{"name": name, "sha256": hashlib.sha256(archive.read(name)).hexdigest(),
                              "size": archive.getinfo(name).file_size} for name in sorted(names)]}


def verify_native_contents(archive: zipfile.ZipFile, expected: dict) -> None:
    admitted = {f"lib/{abi}/{record['name']}": record for abi, records in expected["abis"].items() for record in records}
    actual = {name for name in archive.namelist() if name.startswith("lib/")}
    if actual != set(admitted):
        raise AndroidError("APK contains absent, extra or mixed-ABI native libraries.")
    for name, record in admitted.items():
        if hashlib.sha256(archive.read(name)).hexdigest() != record["sha256"]:
            raise AndroidError("APK native library changed after its preflight inspection.")
    if "openxrProfile" in expected.get("profile", {}):
        lock = expected["toolchain"]["openxrLoader"]
        for abi in expected["abis"]:
            name = f"lib/{abi}/{OPENXR_LOADER_NAME}"
            entry = archive.getinfo(name)
            if (entry.compress_type != zipfile.ZIP_STORED or admitted[name]["sha256"] != lock["abiSha256"][abi]):
                raise AndroidError(f"APK OpenXR loader differs from the locked stored image for {abi}.")


def verify_assets(archive: zipfile.ZipFile, expected: dict) -> None:
    admitted = {ASSET_PREFIX + record["name"]: record for record in expected["assets"]}
    actual = {name for name in archive.namelist() if name.startswith(ASSET_PREFIX)}
    if actual != set(admitted) | {PROVENANCE_ASSET}:
        raise AndroidError("APK runtime assets differ from the declared profile.")
    for name, record in admitted.items():
        if hashlib.sha256(archive.read(name)).hexdigest() != record["sha256"]:
            raise AndroidError("APK runtime asset changed after admission.")
    if "openxrProfile" in expected.get("profile", {}):
        license_record = admitted.get(ASSET_PREFIX + OPENXR_LOADER_LICENSE)
        if (license_record is None
                or license_record["sha256"] != expected["toolchain"]["openxrLoader"]["licenseSha256"]):
            raise AndroidError("APK OpenXR loader license differs from the locked license.")


def assemble(arguments: argparse.Namespace) -> None:
    tools, profile = load_contract(arguments.profile, arguments.abi)
    if "openxrProfile" in profile and arguments.native_root:
        raise AndroidError("The Android OpenXR profile must build its declared native diagnostic host.")
    openxr_loader = read_openxr_loader(arguments.openxr_loader, tools, arguments.abi, arguments.unsigned)
    capability = tools_preflight(tools, arguments.sdk, arguments.ndk, arguments.gradle, not arguments.unsigned)
    workspace = arguments.output.resolve()
    if workspace.exists():
        raise AndroidError("Output already exists; choose a new isolated assembly directory. Existing evidence is preserved.")
    workspace.mkdir(parents=True)
    activity_root = workspace / "game-activity"
    extract_game_activity(arguments.game_activity, activity_root, tools, arguments.abi)
    native_root = build_native(arguments, profile, activity_root, workspace)
    stage = workspace / "gradle-project"
    stage.mkdir()
    native = stage_native(native_root, stage, tools, profile, arguments.abi, openxr_loader.images if openxr_loader else None)
    assets = copy_assets(profile, stage / "assets")
    if openxr_loader is not None:
        license_asset = stage / ASSET_PREFIX / OPENXR_LOADER_LICENSE
        license_asset.parent.mkdir(parents=True, exist_ok=True)
        license_asset.write_bytes(openxr_loader.license)
        assets.append({"name": OPENXR_LOADER_LICENSE, "sha256": digest(license_asset), "size": license_asset.stat().st_size})
    apk_write_project(stage, tools, profile, arguments.abi, arguments.game_activity)
    os.environ["ANDROID_HOME"] = str(arguments.sdk.resolve())
    os.environ["SOURCE_DATE_EPOCH"] = "315532800"
    run_gradle_task(arguments.gradle, "writeDependencyProvenance", stage, workspace / "java-dependencies.log")
    dependencies = stage / ASSET_PREFIX / JAVA_DEPENDENCIES_ASSET
    assets.append({"name": dependencies.name, "sha256": digest(dependencies), "size": dependencies.stat().st_size})
    evidence = provenance(tools, profile, native, assets)
    (stage / PROVENANCE_ASSET).write_bytes(canonical_json(evidence))
    task = "assemble" + profile["configuration"]
    run_gradle_task(arguments.gradle, task, stage, workspace / "gradle.log")
    complete_package(stage, workspace, profile, tools, capability, evidence, arguments.unsigned)


def include_generated_assets(package: Path, evidence: dict) -> None:
    """Bind explicitly declared Gradle-produced runtime assets before canonical alignment/signing."""
    with zipfile.ZipFile(package) as archive:
        archive_limits(archive)
        if archive.read(PROVENANCE_ASSET) != canonical_json(evidence):
            raise AndroidError("Gradle changed the admitted source provenance before final assembly.")
        generated = evidence["profile"]["generatedAssets"]
        ordinary = {ASSET_PREFIX + record["name"] for record in evidence["assets"]}
        expected = ordinary | {ASSET_PREFIX + name for name in generated} | {PROVENANCE_ASSET}
        actual = {name for name in archive.namelist() if name.startswith(ASSET_PREFIX)}
        if actual != expected:
            raise AndroidError("Gradle output has absent or undeclared generated runtime assets.")
        for name in generated:
            payload = archive.read(ASSET_PREFIX + name)
            evidence["assets"].append({"name":name, "sha256":hashlib.sha256(payload).hexdigest(),
                                       "size":len(payload), "origin":"androidGradlePlugin"})


def complete_package(stage: Path, workspace: Path, profile: dict, tools: dict, capability: dict, evidence: dict, unsigned: bool) -> None:
    candidates = list((stage / "build/outputs/apk" / profile["configuration"].lower()).glob("*.apk"))
    if len(candidates) != 1:
        raise AndroidError("Gradle must produce exactly one APK for the admitted ABI set.")
    canonical = workspace / "canonical-unaligned.apk"
    include_generated_assets(candidates[0], evidence)
    canonical_apk(candidates[0], canonical, canonical_json(evidence))
    aligned = workspace / "unsigned.apk"
    android_run([capability["commands"]["zipalign"], "-P", "16", "-f", "4", str(canonical), str(aligned)], workspace, workspace / "align.log")
    package = aligned
    if not unsigned:
        package = workspace / "signed.apk"
        sign_package(aligned, package, tools, capability, workspace)
    android_run([capability["commands"]["zipalign"], "-c", "-P", "16", "4", str(package)], workspace, workspace / "verify-align.log")
    badging = version_output([capability["commands"]["aapt2"], "dump", "badging", str(package)])
    (workspace / "manifest-badging.txt").write_text(badging, encoding="utf-8")
    verify_manifest(badging, profile)
    inspection = inspect_package(package, evidence)
    inspection["signed"] = not unsigned
    (workspace / "inspection.json").write_bytes(canonical_json(inspection))
    (workspace / "provenance.json").write_bytes(canonical_json(evidence))
    summary = {"package": package.name, "sha256": inspection["apkSha256"], "signed": not unsigned}
    if "openxrProfile" in profile:
        summary["openxr"] = profile["openxrProfile"]
    print(json.dumps(summary))


def verify_api_badging(badging: str, field: str, expected: int) -> None:
    values = re.findall(rf"^{field}:'([^']+)'$", badging, re.MULTILINE)
    if values != [str(expected)]:
        raise AndroidError(f"Manifest {field} differs from profile API {expected}: {values}; inspect manifest-badging.txt.")


def verify_manifest(badging: str, profile: dict) -> None:
    package = re.search(r"^package: name='([^']+)' versionCode='([^']+)' versionName='([^']+)'", badging, re.MULTILINE)
    if package is None or package.groups() != (profile["applicationId"], str(profile["versionCode"]), profile["versionName"]):
        raise AndroidError("Assembled manifest identity differs from the declared package profile.")
    verify_api_badging(badging, "(?:minSdkVersion|sdkVersion)", profile["minimumApi"])
    verify_api_badging(badging, "targetSdkVersion", profile["targetApi"])
    permissions = set(re.findall(r"^uses-permission: name='([^']+)'", badging, re.MULTILINE))
    if permissions != set(profile["permissions"]):
        raise AndroidError("Assembled manifest has absent or undeclared permissions.")
    features = set(re.findall(r"^uses-feature: name='([^']+)'", badging, re.MULTILINE))
    if features != set(profile["features"]):
        raise AndroidError("Assembled manifest requires undeclared hardware features.")


def apk_main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", required=True, help="Exact name from android/package-profiles.json.")
    parser.add_argument("--abi", action="append", required=True)
    for name in ("sdk", "ndk", "gradle", "game-activity", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--native-root", type=Path, help="Explicit prebuilt ABI directories; complete closure is inspected before assembly.")
    parser.add_argument("--openxr-loader", type=Path, help="Explicit pinned Khronos loader AAR for the signed OpenXR development profile.")
    parser.add_argument("--unsigned", action="store_true", help="Produce an unsigned qualification candidate; no install/signing claim.")
    arguments = parser.parse_args(argv)
    try:
        assemble(arguments)
    except (OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile) as error:
        print(f"Android preflight: {error}", file=sys.stderr)
        return 1
    return 0


# Android developer deployment

PACKAGE_ACTIVITY = "PackageActivity"
DEPLOY_CAPTURE_BYTES = 256 * 1024
DEPLOY_REPORT_BYTES = 16 * 1024
DEPLOY_CLEANUP_SECONDS = 5
DEPLOY_REPORT_ENTRY = "android/deployment.json"
ADB_CONNECTION_MODES = ("usb", "emulator", "tcpip")


class AndroidDeployError(AndroidError):
    """Stable portable deployment diagnostic with no child output."""

    def __init__(self, code: str):
        super().__init__(code)
        self.code = code


def stop_deploy_process(process: subprocess.Popen, *, leader_exited: bool = False) -> None:
    """Stop only the admitted host child and its isolated POSIX process group."""
    # Keep the leader PID reserved until the group has been signalled. Polling
    # would reap an exited leader even if its descendants still hold stdout.
    if process.returncode is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    except PermissionError:
        # Darwin excludes zombie leaders and can report EPERM once the isolated
        # group has no remaining members signalable by this invocation.
        if sys.platform != "darwin" or not leader_exited:
            raise
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        raise AndroidDeployError("host_process_stop_unconfirmed") from None


def wait_deploy_status(process: subprocess.Popen, deadline: float) -> int:
    """Retire the group of a failed POSIX command before releasing its leader PID."""
    if hasattr(os, "waitid") and hasattr(os, "WNOWAIT"):
        while True:
            exited = os.waitid(os.P_PID, process.pid, os.WEXITED | os.WNOWAIT | os.WNOHANG)
            if exited is not None:
                if exited.si_code != os.CLD_EXITED or exited.si_status != 0:
                    stop_deploy_process(process, leader_exited=True)
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AndroidDeployError("deadline_exceeded")
            time.sleep(min(0.01, remaining))
    return process.wait(timeout=max(0, deadline - time.monotonic()))


@contextmanager
def owned_deploy_process(command: list[str], retain_adb_server: bool) -> Iterator[subprocess.Popen]:
    """Admit an argv command with host-specific ownership of its subprocess tree."""
    argv = admitted_command(command)
    environment = execution_environment()
    if os.name == "nt":
        from dev import CleanupError, owned_windows_process

        try:
            with owned_windows_process(argv, env=environment, capture_output=True,
                                       retain_children_on_success=retain_adb_server) as process:
                yield process
        except CleanupError:
            raise AndroidDeployError("host_process_stop_unconfirmed") from None
        return
    process = subprocess.Popen(
        argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
        shell=False, start_new_session=True, env=environment)
    try:
        yield process
    finally:
        stop_deploy_process(process)


def deploy_process(command: list[str], deadline: float, *, retain_adb_server: bool = False) -> tuple[int, str]:
    """Collect at most one bounded output buffer before a shared absolute deadline."""
    if deadline <= time.monotonic():
        raise AndroidDeployError("deadline_exceeded")
    with owned_deploy_process(command, retain_adb_server) as process:
        return read_deploy_process(process, deadline)


def read_deploy_process(process: subprocess.Popen, deadline: float) -> tuple[int, str]:
    """Read only the owned command's bounded output and terminal status."""
    output: Queue[bytes | OSError] = Queue(maxsize=1)

    def capture() -> None:
        try:
            with process.stdout as pipe:
                output.put(pipe.read(DEPLOY_CAPTURE_BYTES + 1))
        except OSError as error:
            output.put(error)

    reader = Thread(target=capture, daemon=True)
    try:
        reader.start()
        captured = output.get(timeout=max(0, deadline - time.monotonic()))
        if isinstance(captured, OSError):
            raise AndroidDeployError("host_output_unavailable") from None
        if len(captured) > DEPLOY_CAPTURE_BYTES:
            raise AndroidDeployError("host_output_limit")
        status = wait_deploy_status(process, deadline)
        return status, captured.decode("utf-8", errors="replace")
    except (Empty, subprocess.TimeoutExpired):
        raise AndroidDeployError("deadline_exceeded") from None


def deploy_tool(sdk: Path, name: str) -> str:
    """Select a native Android SDK executable without implicit cwd or shell resolution."""
    return str((sdk / (name + (".exe" if os.name == "nt" else ""))).absolute())


@dataclass(frozen=True)
class OpenXRDiagnosticImages:
    """Digests of the verified native diagnostic and bundled loader for one APK ABI."""

    abi: str
    diagnostic_host_sha256: str
    loader_sha256: str

    def report(self) -> dict[str, str]:
        return {"abi": self.abi, "diagnosticHostSha256": self.diagnostic_host_sha256, "loaderSha256": self.loader_sha256}


@dataclass(frozen=True)
class VerifiedAndroidArtifact:
    path: Path
    profile: dict
    apk_sha256: str
    provenance_sha256: str
    source_revision: str
    abis: frozenset[str]
    openxr_images: tuple[OpenXRDiagnosticImages, ...] = ()


def verified_openxr_images(profile: dict, inspection: dict, abis: frozenset[str]) -> tuple[OpenXRDiagnosticImages, ...]:
    """Select only the named OpenXR binaries from the verified signed APK inspection."""
    if "openxrProfile" not in profile:
        return ()
    host_library = f"lib{profile['nativeLibrary']}.so"
    entries = {record["name"]: record["sha256"] for record in inspection["contents"]}
    return tuple(OpenXRDiagnosticImages(abi, entries[f"lib/{abi}/{host_library}"], entries[f"lib/{abi}/{OPENXR_LOADER_NAME}"])
                 for abi in sorted(abis))


def deploy_artifact(artifact: Path, stage: Path, sdk: Path, profile_name: str,
                    java: str, deadline: float) -> VerifiedAndroidArtifact:
    """Recheck the exact signed package bytes and expected launch component before touching a device."""
    declared = json.loads(bounded_bytes(input_path(artifact, "provenance.json"), 1024 * 1024))
    inspection = json.loads(bounded_bytes(input_path(artifact, "inspection.json"), 8 * 1024 * 1024))
    abis = frozenset(declared["abis"])
    tools, profile = load_contract(profile_name, sorted(abis))
    if (declared["schema"] != 1 or declared["profile"] != profile or declared.get("toolchain") != tools
            or inspection.get("signed") is not True):
        raise AndroidDeployError("artifact_contract_mismatch")
    names = sorted(profile["nativeLibraries"])
    assets = [*profile["assets"], *profile["generatedAssets"], JAVA_DEPENDENCIES_ASSET]
    if "openxrProfile" in profile:
        assets.append(OPENXR_LOADER_LICENSE)
    if (any(sorted(record["name"] for record in declared["abis"][abi]) != names for abi in abis)
            or sorted(record["name"] for record in declared["assets"]) != sorted(assets)):
        raise AndroidDeployError("artifact_contract_mismatch")
    revision = declared.get("sourceRevision")
    if not isinstance(revision, str) or not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise AndroidDeployError("artifact_source_revision_unavailable")
    if declared.get("javaDependencyLockSha256") != digest(ROOT / "android/gradle/gradle.lockfile"):
        raise AndroidDeployError("artifact_dependency_lock_stale")
    package = stage / "signed.apk"
    package.write_bytes(bounded_bytes(input_path(artifact, "signed.apk")))
    actual = inspect_package(package, declared)
    if actual | {"signed": True} != inspection:
        raise AndroidDeployError("artifact_inspection_mismatch")
    build_tools = sdk / "build-tools" / tools["buildTools"]
    require_revision(build_tools, tools["buildTools"], "Build tools")
    signing_verifier = input_path(build_tools, "lib/apksigner.jar")
    status, _ = deploy_process([java, "-jar", str(signing_verifier.absolute()), "verify", str(package)], deadline)
    if status:
        raise AndroidDeployError("artifact_signature_invalid")
    status, badging = deploy_process([deploy_tool(build_tools, "aapt2"), "dump", "badging", str(package)], deadline)
    if status:
        raise AndroidDeployError("artifact_manifest_unavailable")
    verify_manifest(badging, profile)
    activity = profile["applicationId"] + "." + PACKAGE_ACTIVITY
    if re.findall(r"^launchable-activity: name='([^']+)'", badging, re.MULTILINE) != [activity]:
        raise AndroidDeployError("artifact_activity_mismatch")
    return VerifiedAndroidArtifact(package, profile, actual["apkSha256"], actual["provenanceSha256"], revision, abis,
                                   verified_openxr_images(profile, actual, abis))


@dataclass(frozen=True)
class AdmittedAdbDevice:
    """One explicitly selected transport, shared by every command in the operation."""

    executable: str
    serial: str
    transport: int
    deadline: float
    connection: str

    def run(self, *arguments: str, missing_process: bool = False) -> str:
        status, output = deploy_process([self.executable, "-t", str(self.transport), *arguments], self.deadline)
        if any(line.startswith("* daemon") for line in output.splitlines()):
            raise AndroidDeployError("device_transport_changed")
        if status:
            if missing_process and status == 1 and not output.strip():
                return ""
            lower = output.lower()
            if "unauthorized" in lower:
                raise AndroidDeployError("device_unauthorized")
            if "offline" in lower or "not found" in lower or "no devices" in lower or "disconnected" in lower:
                raise AndroidDeployError("device_disconnected")
            raise AndroidDeployError("device_command_failed")
        return "\n".join(output.splitlines()).strip()

    def process_id(self, application: str) -> int | None:
        output = self.run("shell", "pidof", application, missing_process=True)
        if not output:
            return None
        pid = deploy_number(output, 2**31 - 1, "application_process_unavailable")
        if not pid:
            raise AndroidDeployError("application_process_unavailable")
        return pid


def adb_connection_mode(serial: str, details: list[str]) -> str:
    """Classify the selected ADB inventory row without exporting raw device identifiers."""
    modes = []
    if any(field.startswith("usb:") and field != "usb:" for field in details):
        modes.append("usb")
    if re.fullmatch(r"emulator-[0-9]{1,5}", serial):
        modes.append("emulator")
    tcp = re.fullmatch(r"(?:[A-Za-z0-9][A-Za-z0-9.-]*|\[[a-fA-F0-9:%.]+\]):([0-9]{1,5})", serial)
    if ((tcp and 0 < int(tcp.group(1)) <= 65535)
            or re.fullmatch(r"adb-[A-Za-z0-9-]+\._adb-tls-connect\._tcp(?:\.local)?", serial)):
        modes.append("tcpip")
    return modes[0] if len(modes) == 1 else "unknown"


def admit_adb_device(executable: str, serial: str, deadline: float) -> AdmittedAdbDevice:
    """Resolve a serial to one current ADB transport. Reconnects require a new operation."""
    status, output = deploy_process([executable, "devices", "-l"], deadline, retain_adb_server=True)
    if status:
        raise AndroidDeployError("device_inventory_unavailable")
    matches = [fields for line in output.splitlines() if (fields := line.split()) and fields[0] == serial]
    if len(matches) != 1 or len(matches[0]) < 2:
        raise AndroidDeployError("device_unavailable")
    if matches[0][1] == "unauthorized":
        raise AndroidDeployError("device_unauthorized")
    if matches[0][1] != "device":
        raise AndroidDeployError("device_disconnected")
    identities = [field.removeprefix("transport_id:") for field in matches[0][2:] if field.startswith("transport_id:")]
    if len(identities) != 1:
        raise AndroidDeployError("device_transport_unavailable")
    transport = deploy_number(identities[0], 2**63 - 1, "device_transport_unavailable")
    if transport == 0:
        raise AndroidDeployError("device_transport_unavailable")
    device = AdmittedAdbDevice(executable, serial, transport, deadline, adb_connection_mode(serial, matches[0][2:]))
    if device.run("get-serialno") != serial:
        raise AndroidDeployError("device_transport_changed")
    return device


def deploy_number(output: str, maximum: int, code: str) -> int:
    """Admit bounded decimal fields without forwarding device text."""
    if not re.fullmatch(r"[0-9]{1,20}", output) or int(output) > maximum:
        raise AndroidDeployError(code)
    return int(output)


def deploy_device_identity(device: AdmittedAdbDevice, minimum_api: int, abis: frozenset[str]) -> tuple[int, int, str]:
    if device.run("get-state") != "device":
        raise AndroidDeployError("device_unavailable")
    user = deploy_number(device.run("shell", "am", "get-current-user"), 2**31 - 1, "device_user_unavailable")
    api = deploy_number(device.run("shell", "getprop", "ro.build.version.sdk"), 9999, "device_api_unavailable")
    abi = device.run("shell", "getprop", "ro.product.cpu.abi")
    if api < minimum_api or abi not in abis:
        raise AndroidDeployError("device_profile_unsupported")
    return user, api, abi


def deploy_device_build_digest(device: AdmittedAdbDevice) -> str:
    """Bind XR evidence to the Android system build without exporting the raw fingerprint."""
    fingerprint = device.run("shell", "getprop", "ro.build.fingerprint")
    if re.fullmatch(r"[!-~]{1,256}", fingerprint) is None:
        raise AndroidDeployError("device_build_unverifiable")
    return hashlib.sha256(fingerprint.encode("ascii")).hexdigest()


def deploy_verified_install(device: AdmittedAdbDevice, application: str, user: str, digest_value: str) -> None:
    """Require the same installed package digest before launch and evidence admission."""
    reported = device.run("shell", "pm", "path", "--user", user, application)
    match = re.fullmatch(r"package:(/[A-Za-z0-9_./=~+@-]{1,1024}/base\.apk)", reported)
    if not match or any(part in (".", "..") for part in match.group(1).split("/")):
        raise AndroidDeployError("installed_artifact_unverifiable")
    filename = match.group(1)
    checksum = device.run("shell", "toybox", "sha256sum", filename)
    if re.fullmatch(r"[a-f0-9]{64}  " + re.escape(filename), checksum) is None:
        raise AndroidDeployError("installed_artifact_unverifiable")
    if checksum.split()[0] != digest_value:
        raise AndroidDeployError("installed_artifact_stale")


STATE_FIELDS = ("lifecycleRevision", "activity", "state", "process", "window", "presentation", "factsRevision")
STATE_FLAGS = ("nativeAvailable", "ready", "hasWindow", "hasPresentation")
FRAME_FIELDS = ("attempts", "presentAccepted", "beginPending", "executePending", "presentPending")
STATE_LINE = re.compile(
    r"host-state " + " ".join(fr"{name}=(?P<{name}>[0-9]{{1,20}})" for name in STATE_FIELDS)
    + r" extent=(?P<width>[0-9]{1,8})x(?P<height>[0-9]{1,8}) "
    + " ".join(fr"{name}=(?P<{name}>[01])" for name in STATE_FLAGS)
    + r" attempts=(?P<attempts>[0-9]{1,20}) presentAccepted=(?P<presentAccepted>[0-9]{1,20})")
FRAME_LINE = re.compile(
    r"host-frame stage=(?P<stage>begin|execute|present) outcome=(?P<outcome>accepted|backpressure|failed) "
    + " ".join(fr"{name}=(?P<{name}>[0-9]{{1,20}})" for name in FRAME_FIELDS) + r" completion=unobserved")
PLATFORM_STATE_LINES = {
    "thermal": re.compile(
        r"host-thermal revision=(?P<revision>[1-9][0-9]{0,18}) "
        r"state=(?P<state>pending|inactive|unsupported|unavailable|none|light|moderate|severe|critical|emergency|shutdown)"),
    "memory": re.compile(
        r"host-memory revision=(?P<revision>[1-9][0-9]{0,18}) "
        r"state=(?P<state>inactive|unsupported|unavailable|normal|moderate|low|critical)"),
    "power": re.compile(
        r"host-power revision=(?P<revision>[1-9][0-9]{0,18}) "
        r"state=(?P<state>inactive|unsupported|unavailable|enabled|disabled)"),
    "battery": re.compile(
        r"host-battery revision=(?P<revision>[1-9][0-9]{0,18}) "
        r"state=(?P<state>inactive|unsupported|unavailable|charging|discharging|not-charging|full) "
        r"percent=(?P<percent>-1|0|[1-9][0-9]{0,2})"),
    "pacing": re.compile(
        r"host-pacing revision=(?P<revision>[1-9][0-9]{0,18}) "
        r"state=(?P<state>baseline|power-conservation|thermal-conservation|memory-conservation)"
        r"(?: intervalMs=(?P<intervalMs>[1-9][0-9]{0,9}) attempts=(?P<attempts>0|[1-9][0-9]{0,19}))?"),
    "refresh": re.compile(
        r"host-refresh revision=(?P<revision>[1-9][0-9]{0,18}) "
        r"state=(?P<state>inactive|unsupported|unavailable|observed) milliHz=(?P<milliHz>0|[1-9][0-9]{0,9})"),
}
OPENXR_LINE = re.compile(
    r"host-openxr abi=(arm64-v8a|x86_64) stage=(host-application|bundled-loader|loader-dispatch|loader-initialization|extension-admission|"
    r"instance-creation|system-discovery|runtime-properties|instance-retirement|discovered) "
    r"result=(0|-?[1-9][0-9]{0,9}) runtimeMajor=([0-9]{1,5}) runtimeMinor=([0-9]{1,5}) runtimePatch=([0-9]{1,10})")


LOG_ENTRY_TIME = re.compile(
    r"\s*([0-9]{1,12}\.[0-9]{9}) ([IWEF])/(HoroPackageQualification|AndroidRuntime)\s*"
    r"\(\s*([0-9]{1,10}|u[0-9]{1,5}_a[0-9]{1,5}):\s*([0-9]{1,10})\): (.*)")


def log_epoch_nanoseconds(value: str) -> int:
    if not re.fullmatch(r"[0-9]{1,12}\.[0-9]{9}", value):
        raise AndroidDeployError("device_log_clock_unavailable")
    seconds, nanos = value.split(".")
    return int(seconds) * 1_000_000_000 + int(nanos)


@dataclass(frozen=True)
class AndroidLogClock:
    """One device realtime sample bracketed by the host's monotonic clock."""

    nanoseconds: int
    host_before: int
    host_after: int

    @classmethod
    def read(cls, device: AdmittedAdbDevice) -> AndroidLogClock:
        before = time.monotonic_ns()
        sampled = device.run("shell", "toybox", "date", "+%s.%N")
        return cls(log_epoch_nanoseconds(sampled), before, time.monotonic_ns())

    def require_continuity(self, initial: AndroidLogClock) -> None:
        elapsed = self.nanoseconds - initial.nanoseconds
        lower = max(0, self.host_before - initial.host_after)
        upper = self.host_after - initial.host_before
        if elapsed <= 0 or not lower <= elapsed <= upper:
            raise AndroidDeployError("device_log_clock_changed")


@dataclass(frozen=True)
class AndroidDiagnosticWindow:
    """Keep the clock fence and the minimum accepted diagnostic timestamp distinct."""

    start: AndroidLogClock
    evidence_after: int


@dataclass(frozen=True)
class DeployedProcess:
    pid: int
    uid: int
    start_ticks: int


def openxr_log_evidence(message: str) -> dict:
    match = OPENXR_LINE.fullmatch(message)
    if match is None:
        raise AndroidDeployError("openxr_diagnostic_invalid")
    abi, stage, result, major, minor, patch = match.groups()
    result, major, minor, patch = map(int, (result, major, minor, patch))
    if not -2**31 <= result < 2**31 or major >= 2**16 or minor >= 2**16 or patch >= 2**32:
        raise AndroidDeployError("openxr_diagnostic_invalid")
    evidence = {"abi": abi, "stage": stage, "result": result}
    if stage == "discovered":
        if result == 0 and major:
            return evidence | {"runtimeVersion": {"major": major, "minor": minor, "patch": patch}}
    elif result != 0 and not (major or minor or patch):
        return evidence
    raise AndroidDeployError("openxr_diagnostic_invalid")


def expected_openxr_runtime_version(value: str) -> dict[str, int]:
    match = re.fullmatch(r"([1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,9})", value)
    if match is None:
        raise argparse.ArgumentTypeError("Expected a numeric OpenXR runtime version, such as 1.1.63.")
    major, minor, patch = map(int, match.groups())
    if major >= 2**16 or minor >= 2**16 or patch >= 2**32:
        raise argparse.ArgumentTypeError("Expected a version that fits the OpenXR version fields.")
    return {"major": major, "minor": minor, "patch": patch}


def qualification_uid_log_entries(log: str, expected_uid: int, after: int,
                                  through: int) -> Iterator[tuple[int, str, str, int, str]]:
    """Read log fields for one installed UID inside the verified device-clock interval."""
    user, app = divmod(expected_uid, 100_000)
    accepted_uids = {str(expected_uid)}
    if app >= 10_000:
        accepted_uids.add(f"u{user}_a{app - 10_000}")
    for line in log.splitlines():
        header = LOG_ENTRY_TIME.fullmatch(line)
        if not header:
            continue
        stamp, priority, tag, uid, pid, message = header.groups()
        if uid not in accepted_uids:
            continue
        logged = log_epoch_nanoseconds(stamp)
        if after < logged <= through:
            yield logged, priority, tag, int(pid), message


def process_log_entries(log: str, process: DeployedProcess, after: int, through: int) -> Iterator[tuple[int, str, str, str]]:
    """Yield only entries from the exact UID, PID and admitted time interval."""
    for logged, priority, tag, pid, message in qualification_uid_log_entries(log, process.uid, after, through):
        if pid == process.pid:
            yield logged, priority, tag, message


def platform_state(category: str, state: re.Match) -> dict | None:
    """Project a bounded platform observation with only available measurements."""
    revision = int(state["revision"])
    if not 0 < revision < 2**63:
        return None
    value = {"revision": revision, "state": state["state"]}
    if category == "refresh":
        rate = int(state["milliHz"])
        if rate >= 2**31 or (rate > 0) != (value["state"] == "observed"):
            return None
        if rate:
            value["milliHz"] = rate
    elif category == "battery":
        percent = int(state["percent"])
        measured = value["state"] in ("charging", "discharging", "not-charging", "full")
        if percent > 100 or (percent >= 0) != measured:
            return None
        if measured:
            value["percent"] = percent
    elif category == "pacing" and state["intervalMs"] is not None:
        interval, attempts = int(state["intervalMs"]), int(state["attempts"])
        if interval >= 2**31 or attempts >= 2**64:
            return None
        value.update(intervalMs=interval, attempts=attempts)
    return value


class AndroidPressureTimeline:
    """Retains bounded, redacted transitions across overlapping logcat windows."""

    def __init__(self, after: int):
        self.after = after
        self.through = after
        self.transitions = {}
        self.retained_bytes = 0
        self.truncated = False

    def record(self, category: str, logged: int, fields: dict) -> None:
        # Lifecycle facts can change without a new committed lifecycle revision.
        identity = (category, fields["revision"]) if "revision" in fields else (category, logged, tuple(fields.items()))
        transition = {"category": category, "elapsedNs": logged - self.after, **fields}
        if identity in self.transitions:
            self.truncated |= self.transitions[identity] != transition
            return
        size = len(canonical_json(transition)) + 1
        if len(self.transitions) == DEPLOY_TRANSITION_LIMIT or self.retained_bytes + size > DEPLOY_TRANSITION_BYTES:
            self.truncated = True
            return
        self.transitions[identity] = transition
        self.retained_bytes += size

    def finish_poll(self, through: int, admitted_lines: int, first: int | None) -> None:
        if admitted_lines >= DEPLOY_LOG_TAIL_LINES and first is not None and first > self.through:
            self.truncated = True
        self.through = through

    def report(self, duration: int, complete: bool) -> dict:
        return {"durationSeconds": duration, "complete": complete, "truncated": self.truncated,
                "transitions": sorted(self.transitions.values(), key=lambda value: value["elapsedNs"])}


def record_platform_transition(evidence: dict, message: str, logged: int, categories: tuple[str, ...],
                               timeline: AndroidPressureTimeline | None) -> None:
    for category, pattern in PLATFORM_STATE_LINES.items():
        if category not in categories or (state := pattern.fullmatch(message)) is None:
            continue
        value = platform_state(category, state)
        if value is None:
            return
        if timeline is not None:
            timeline.record(category, logged, value)
        if value["revision"] > evidence.get(category, {}).get("revision", 0):
            evidence[category] = value
        return


def record_render_evidence(evidence: dict, latest: dict, message: str, logged: int, categories: tuple[str, ...],
                           timeline: AndroidPressureTimeline | None) -> None:
    for category, pattern in (("lifecycle", STATE_LINE), ("renderer", FRAME_LINE)):
        if category not in categories or (match := pattern.fullmatch(message)) is None:
            continue
        fields = {name: (int(value) if value.isdecimal() else value) for name, value in match.groupdict().items()}
        if any(isinstance(value, int) and value >= 2**64 for value in fields.values()):
            return
        if timeline is not None and category == "lifecycle":
            timeline.record(category, logged, fields)
        if logged >= latest.get(category, 0):
            evidence[category] = fields
            latest[category] = logged
        return


def package_log_evidence(log: str, process: DeployedProcess, after: int, through: int, categories: tuple[str, ...],
                         timeline: AndroidPressureTimeline | None = None) -> dict:
    """Project named telemetry only from the exact UID, PID and admitted time interval."""
    evidence = {"crashObserved": False}
    latest = {}
    first = None
    admitted_lines = 0
    for logged, priority, tag, message in process_log_entries(log, process, after, through):
        if timeline is not None:
            first = logged if first is None else min(first, logged)
            admitted_lines += 1
        if tag == "AndroidRuntime":
            evidence["crashObserved"] |= priority in ("E", "F") and message.startswith("FATAL EXCEPTION:")
            continue
        if tag != "HoroPackageQualification" or priority != "I":
            continue
        if "entry" in categories and message == DEPLOY_ENTRY_MARKER:
            evidence["entry"] = {"input": "accepted"}
            continue
        if "openxr" in categories and message.startswith("host-openxr "):
            if "openxr" in evidence:
                raise AndroidDeployError("openxr_diagnostic_ambiguous")
            evidence["openxr"] = openxr_log_evidence(message)
            continue
        record_platform_transition(evidence, message, logged, categories, timeline)
        record_render_evidence(evidence, latest, message, logged, categories, timeline)
    if timeline is not None:
        timeline.finish_poll(through, admitted_lines, first)
    return evidence


def deployed_uid(device: AdmittedAdbDevice, application: str, user: str) -> int:
    inventory = device.run("shell", "cmd", "package", "list", "packages", "--user", user, "-U", application)
    matches = re.findall(r"^package:" + re.escape(application) + r" uid:([0-9]{1,10})$", inventory, re.MULTILINE)
    if len(matches) != 1:
        raise AndroidDeployError("installed_identity_unverifiable")
    return deploy_number(matches[0], 2**32 - 1, "installed_identity_unverifiable")


def process_start_ticks(device: AdmittedAdbDevice, pid: int) -> int:
    """Read Linux stat field 22 after separating the parenthesized process name."""
    observed = device.run("shell", "toybox", "cat", f"/proc/{pid}/stat")
    prefix, _, remainder = observed.rpartition(") ")
    fields = remainder.split()
    if not prefix.startswith(f"{pid} (") or len(fields) <= 19:
        raise AndroidDeployError("application_process_unavailable")
    return deploy_number(fields[19], 2**64 - 1, "application_process_unavailable")


def deployed_process(device: AdmittedAdbDevice, application: str, uid: int) -> DeployedProcess:
    """Capture the exact live process incarnation for one already verified package user."""
    pid = device.process_id(application)
    if not pid:
        raise AndroidDeployError("application_process_unavailable")
    started = process_start_ticks(device, pid)
    identity = device.run("shell", "toybox", "ps", "-p", str(pid), "-o", "UID")
    matched = re.fullmatch(r"UID\n\s*([0-9]{1,10})", identity)
    if not matched:
        raise AndroidDeployError("application_process_unavailable")
    if uid != int(matched.group(1)):
        raise AndroidDeployError("application_process_identity_mismatch")
    if process_start_ticks(device, pid) != started:
        raise AndroidDeployError("application_process_changed")
    return DeployedProcess(pid, uid, started)


def read_deployed_process_log(device: AdmittedAdbDevice, process: DeployedProcess, start: AndroidLogClock) -> tuple[str, int]:
    """Read bounded logcat output and validate the device clock interval."""
    raw = device.run("logcat", f"--pid={process.pid}", "-v", "time,epoch,nsec,uid", "-t", str(DEPLOY_LOG_TAIL_LINES), "-s",
                     "HoroPackageQualification:I", "AndroidRuntime:E")
    end = AndroidLogClock.read(device)
    end.require_continuity(start)
    return raw, end.nanoseconds


def deploy_diagnostics(device: AdmittedAdbDevice, application: str, process: DeployedProcess, window: AndroidDiagnosticWindow,
                       categories: tuple[str, ...], timeline: AndroidPressureTimeline | None = None) -> dict:
    """Collect bounded markers from the exact process captured by this launch."""
    raw, through = read_deployed_process_log(device, process, window.start)
    facts = package_log_evidence(raw, process, window.evidence_after, through, categories, timeline)
    if facts["crashObserved"] and device.process_id(application) is None:
        return facts
    if deployed_process(device, application, process.uid) != process:
        raise AndroidDeployError("application_process_changed")
    return facts


def await_deploy_diagnostics(device: AdmittedAdbDevice, application: str, process: DeployedProcess,
                             window: AndroidDiagnosticWindow, profile: dict, observe_seconds: int = 0) -> tuple[dict, bool]:
    """Wait for actual evidence from the selected profile on the verified ADB transport and process."""
    openxr = "openxrProfile" in profile
    if openxr:
        required = ("openxr",)
    elif profile["renderer"] == "none":
        required = ("entry",)
    else:
        required = ("lifecycle", "renderer")
    categories = (*required, *PLATFORM_STATE_LINES) if profile["renderer"] == "vulkan-android" else required
    timeline = AndroidPressureTimeline(window.evidence_after) if observe_seconds else None
    end = time.monotonic() + observe_seconds if timeline is not None else device.deadline
    facts = {"crashObserved": False}
    applicable = False
    while time.monotonic() < device.deadline:
        observed = deploy_diagnostics(device, application, process, window, categories, timeline)
        for category in PLATFORM_STATE_LINES:
            if (state := observed.get(category)) and state["revision"] <= facts.get(category, {}).get("revision", 0):
                observed.pop(category)
        facts.update(observed)
        applicable = all(key in facts for key in required)
        ready = applicable if timeline is None else time.monotonic() >= end
        if facts["crashObserved"] or ready:
            break
        time.sleep(min(0.5 if timeline is not None else 0.1, max(0, min(end, device.deadline) - time.monotonic())))
    if timeline is not None:
        complete = time.monotonic() >= end and not facts["crashObserved"]
        facts["observationWindow"] = timeline.report(observe_seconds, complete)
        return facts, applicable and complete and not timeline.truncated
    if openxr and not applicable and not facts["crashObserved"]:
        raise AndroidDeployError("openxr_diagnostic_timeout")
    return facts, applicable


@dataclass(frozen=True)
class AndroidLaunchRecovery:
    """Device observations needed to attribute an interrupted launch."""

    uid: int
    started: AndroidLogClock
    nonce: str
    deadline_ms: int | None


@dataclass(frozen=True)
class AndroidLaunchReceipt:
    process: DeployedProcess
    deadline_timestamp: int | None


@dataclass
class AndroidPackageLaunch:
    """Tracks the issued launch and its attributable process for rollback."""

    device: AdmittedAdbDevice
    application: str
    user: str
    digest: str
    openxr_diagnostic: bool = False
    process: DeployedProcess | None = None
    attempted: bool = False
    recovery: AndroidLaunchRecovery | None = None
    retired_crash: bool = False

    def start(self) -> tuple[DeployedProcess, AndroidDiagnosticWindow]:
        uid = deployed_uid(self.device, self.application, self.user)
        if self.device.process_id(self.application) is not None:
            raise AndroidDeployError("application_already_running")
        component = f"{self.application}/{self.application}.{PACKAGE_ACTIVITY}"
        start = AndroidLogClock.read(self.device)
        nonce = secrets.token_hex(16)
        launch_options = ["--es", DEPLOY_LAUNCH_EXTRA, nonce]
        deadline_ms = None
        if self.openxr_diagnostic:
            remaining = math.ceil((self.device.deadline - time.monotonic()) * 1000)
            if remaining <= 0:
                raise AndroidDeployError("deadline_exceeded")
            deadline_ms = min(remaining, DEPLOY_MAX_SECONDS * 1000)
            launch_options.extend(("--ei", DEPLOY_DEADLINE_EXTRA, str(deadline_ms)))
        self.recovery = AndroidLaunchRecovery(uid, start, nonce, deadline_ms)
        self.attempted = True
        launched = self.device.run("shell", "am", "start", "-W", "--user", self.user, "-n", component, *launch_options)
        allowed_components = {component, f"{self.application}/.{PACKAGE_ACTIVITY}"}
        states = re.findall(r"^LaunchState: ([A-Z]+)$", launched, re.MULTILINE)
        activities = re.findall(r"^Activity: (\S+)$", launched, re.MULTILINE)
        exact_activity = len(activities) == 1 and activities[0] in allowed_components
        not_started = "Warning: Activity not started" in launched
        if (re.findall(r"^Status: ([a-z]+)$", launched, re.MULTILINE) != ["ok"]
                or not exact_activity or states not in ([], ["COLD"]) or not_started):
            # Allow cleanup only when the device still reported this exact cold launch.
            # Rollback must prove the private receipt and current process before stopping it.
            if not exact_activity or states != ["COLD"] or not_started:
                self.recovery = None
            raise AndroidDeployError("launch_unconfirmed")
        try:
            receipt = self.recover_process(self.device, uid)
            self.process = receipt.process
        except AndroidDeployError as error:
            if (self.openxr_diagnostic and states == ["COLD"] and error.code == "application_process_unavailable"
                    and self.device.process_id(self.application) is None and self.confirm_retired_crash()):
                self.retired_crash = True
                raise AndroidDeployError("application_crashed") from None
            raise
        # Keep the exact process for cleanup even if an old APK did not arm its Activity deadline.
        evidence_after = start.nanoseconds
        if self.openxr_diagnostic:
            if receipt.deadline_timestamp is None:
                raise AndroidDeployError("openxr_launch_deadline_unconfirmed")
            evidence_after = receipt.deadline_timestamp
        return self.process, AndroidDiagnosticWindow(start, evidence_after)

    def recover_process(self, device: AdmittedAdbDevice, uid: int) -> AndroidLaunchReceipt:
        """Prove that this operation started a first Activity in the live package process."""
        recovery = self.recovery
        if recovery is None or recovery.uid != uid:
            raise AndroidDeployError("launch_process_unattributed")
        process = deployed_process(device, self.application, uid)
        log, through = read_deployed_process_log(device, process, recovery.started)
        marker = f"{DEPLOY_LAUNCH_MARKER}{recovery.nonce} startTicks={process.start_ticks}"
        deadline_marker = (f"{DEPLOY_DEADLINE_MARKER}{recovery.nonce} startTicks={process.start_ticks}"
                           f" milliseconds={recovery.deadline_ms}")
        timestamps = []
        deadline_timestamps = []
        for logged, priority, tag, message in process_log_entries(log, process, recovery.started.nanoseconds, through):
            if priority != "I" or tag != "HoroPackageQualification":
                continue
            if message == marker:
                timestamps.append(logged)
            elif recovery.deadline_ms is not None and message == deadline_marker:
                deadline_timestamps.append(logged)
        if len(timestamps) != 1:
            raise AndroidDeployError("launch_process_unattributed")
        if deployed_process(device, self.application, uid) != process:
            raise AndroidDeployError("application_process_changed")
        confirmed_deadline = (deadline_timestamps[0]
                              if len(deadline_timestamps) == 1 and deadline_timestamps[0] > timestamps[0] else None)
        return AndroidLaunchReceipt(process, confirmed_deadline)

    def confirm_retired_crash(self) -> bool:
        """Read exact app receipts for a cold OpenXR Activity that exited before PID admission."""
        recovery = self.recovery
        if recovery is None:
            return False
        raw = self.device.run("logcat", "-v", "time,epoch,nsec,uid", "-t", "200", "-s", "HoroPackageQualification:I")
        through = AndroidLogClock.read(self.device)
        through.require_continuity(recovery.started)
        markers = {
            name: re.compile(re.escape(prefix + recovery.nonce) + r" startTicks=([1-9][0-9]{0,19})")
            for name, prefix in (("launch", DEPLOY_LAUNCH_MARKER), ("crash", DEPLOY_LAUNCH_CRASH_MARKER))
        }
        receipts = {}
        for logged, priority, tag, pid, message in qualification_uid_log_entries(
                raw, recovery.uid, recovery.started.nanoseconds, through.nanoseconds):
            if priority != "I" or tag != "HoroPackageQualification" or not 0 < pid < 2**31:
                continue
            for name, pattern in markers.items():
                if (match := pattern.fullmatch(message)) and int(match[1]) < 2**64:
                    if name in receipts:
                        return False
                    receipts[name] = (logged, pid, int(match[1]))
        launch, crash = receipts.get("launch"), receipts.get("crash")
        return bool(launch and crash and launch[0] < crash[0] and launch[1:] == crash[1:]
                    and self.device.process_id(self.application) is None)

    def rollback(self) -> dict:
        """Use a separate deadline to stop only a provably attributed package process."""
        if self.retired_crash:
            return {"status": "already_exited"}
        if self.process is None and self.recovery is None:
            return {"status": "unconfirmed", "reason": "launch_process_unattributed"}
        device = AdmittedAdbDevice(self.device.executable, self.device.serial, self.device.transport,
                                   time.monotonic() + DEPLOY_CLEANUP_SECONDS, self.device.connection)
        try:
            if device.run("get-serialno") != device.serial:
                raise AndroidDeployError("device_transport_changed")
            if device.process_id(self.application) is None:
                if self.process is None:
                    raise AndroidDeployError("launch_process_unattributed")
                return {"status": "already_exited"}
            deploy_verified_install(device, self.application, self.user, self.digest)
            uid = deployed_uid(device, self.application, self.user)
            if self.process is None:
                self.recover_process(device, uid)
            elif uid != self.process.uid or deployed_process(device, self.application, uid) != self.process:
                raise AndroidDeployError("application_process_changed")
            device.run("shell", "am", "force-stop", "--user", self.user, self.application)
            if device.process_id(self.application) is not None:
                raise AndroidDeployError("application_stop_unconfirmed")
            return {"status": "stopped"}
        except AndroidDeployError as error:
            return {"status": "unconfirmed", "reason": error.code}
        except (KeyboardInterrupt, OSError):
            return {"status": "unconfirmed", "reason": "cleanup_interrupted"}


def android_deploy_host() -> str:
    """Return the closed host OS/architecture identity used in deployment evidence."""
    system, machine = host_platform.system(), host_platform.machine().lower()
    return next((name for name, (os_name, machines) in HOSTS.items()
                 if os_name == system and machine in {value.lower() for value in machines}), "unknown")


def validate_deploy_bundle(path: Path) -> None:
    """Require an absent absolute ZIP output under a stable existing directory."""
    try:
        valid = (path.is_absolute() and path.suffix.lower() == ".zip" and not os.path.lexists(path)
                 and path.parent.is_dir() and path.parent.resolve(strict=True) == path.parent)
    except (OSError, RuntimeError, ValueError):
        raise AndroidDeployError("diagnostic_bundle_destination_invalid") from None
    if not valid:
        raise AndroidDeployError("diagnostic_bundle_destination_invalid")


def write_deploy_bundle(path: Path, report: dict) -> None:
    """Publish a complete report and digest manifest atomically without replacing anything."""
    data = canonical_json(report)
    if len(data) > DEPLOY_REPORT_BYTES:
        raise AndroidDeployError("diagnostic_bundle_report_limit")
    manifest = canonical_json({"schemaVersion": 1, "metadata": {"kind": "android-deployment"}, "missingOptional": [],
                               "files": [{"path": DEPLOY_REPORT_ENTRY, "bytes": len(data),
                                          "sha256": hashlib.sha256(data).hexdigest()}]})
    try:
        with tempfile.TemporaryDirectory(prefix=".horo-android-diagnostics-", dir=path.parent) as scratch:
            staged = Path(scratch) / "bundle.zip"
            with staged.open("xb") as output:
                with zipfile.ZipFile(output, "w") as bundle:
                    bundle.writestr(zipfile.ZipInfo("manifest.json"), manifest)
                    bundle.writestr(zipfile.ZipInfo(DEPLOY_REPORT_ENTRY), data)
                output.flush()
                os.fsync(output.fileno())
            os.link(staged, path)
    except OSError:
        raise AndroidDeployError("diagnostic_bundle_write_failed") from None


def check_deployment_apk(artifact: dict, expected_sha256: str | None) -> None:
    """Compare the verified artifact to an independently selected APK before device access."""
    if expected_sha256 is None:
        return
    artifact["expectedApkSha256"] = expected_sha256
    if artifact["apkSha256"] != expected_sha256:
        raise AndroidDeployError("deployment_apk_digest_mismatch")


def select_openxr_device_images(artifact: VerifiedAndroidArtifact, abi: str) -> dict[str, str]:
    """Report the two verified native images for the selected device ABI."""
    selected = next((images for images in artifact.openxr_images if images.abi == abi), None)
    if selected is None:
        raise AndroidDeployError("openxr_device_images_unavailable")
    return selected.report()


def check_openxr_discovery(runtime: dict, observation: dict, abi: str) -> None:
    """Admit the exact cold-process runtime observation without claiming a device smoke result."""
    if observation["abi"] != abi:
        raise AndroidDeployError("openxr_diagnostic_abi_mismatch")
    if observation["stage"] != "discovered":
        runtime["status"] = "cold_discovery_failed"
        raise AndroidDeployError("openxr_cold_discovery_failed")
    if "expectedVersion" in runtime and observation["runtimeVersion"] != runtime["expectedVersion"]:
        runtime["status"] = "version_mismatch"
        raise AndroidDeployError("openxr_runtime_version_mismatch")
    runtime["status"] = "cold_system_discovered"


def android_deploy_main(argv=None) -> int:
    """Install and launch one declared signed Android artifact, then emit a redacted JSON result."""
    parser = argparse.ArgumentParser(description=android_deploy_main.__doc__)
    parser.add_argument("--artifact", required=True, type=Path, help="Completed Android package output directory.")
    parser.add_argument("--profile", required=True, help="Exact profile used to assemble the artifact.")
    parser.add_argument("--sdk", required=True, type=Path, help="Explicit Android SDK root.")
    parser.add_argument("--device", required=True, help="Explicit connected ADB serial; no default selection.")
    parser.add_argument("--host", choices=PLATFORMS, help="Expected deployment host OS and architecture.")
    parser.add_argument("--connection", choices=ADB_CONNECTION_MODES, help="Expected connection mode for the selected serial.")
    parser.add_argument("--runtime", choices=("none", "openxr"), default="none",
                        help="Explicit runtime intent. OpenXR requires host and connection selections.")
    parser.add_argument("--expect-runtime-version", type=expected_openxr_runtime_version,
                        help="Require this exact OpenXR runtime version from the cold process.")
    parser.add_argument("--expect-apk-sha256", help="Require the exact verified APK SHA-256 before contacting the device.")
    parser.add_argument("--expect-device-build-sha256", help="Require the SHA-256 of the selected Android build fingerprint for OpenXR.")
    parser.add_argument("--java", default="java", help="Native Java executable for the locked signature verifier.")
    parser.add_argument("--timeout", type=int, default=120,
                        help=f"Total host command deadline in seconds (1-{DEPLOY_MAX_SECONDS}).")
    parser.add_argument("--observe-seconds", type=int, default=None,
                        help=f"Collect Vulkan pressure transitions for 1-{DEPLOY_OBSERVATION_MAX_SECONDS} seconds after launch.")
    parser.add_argument("--diagnostic-bundle", type=Path, help="New absolute ZIP path for redacted deployment evidence.")
    arguments = parser.parse_args(argv)
    if (not re.fullmatch(r"[A-Za-z0-9\[][A-Za-z0-9_.:@%+\[\]-]{0,127}", arguments.device)
            or not 1 <= arguments.timeout <= DEPLOY_MAX_SECONDS):
        parser.error(f"Supply a valid explicit ADB serial and a deadline of 1-{DEPLOY_MAX_SECONDS} seconds.")
    if arguments.expect_apk_sha256 is not None and not re.fullmatch(r"[a-f0-9]{64}", arguments.expect_apk_sha256):
        parser.error("--expect-apk-sha256 requires 64 lowercase hexadecimal digits.")
    if arguments.expect_runtime_version is not None and arguments.runtime != "openxr":
        parser.error("--expect-runtime-version requires --runtime openxr.")
    if arguments.observe_seconds is not None and not 1 <= arguments.observe_seconds <= DEPLOY_OBSERVATION_MAX_SECONDS:
        parser.error(f"--observe-seconds must be from 1 to {DEPLOY_OBSERVATION_MAX_SECONDS}.")
    if arguments.expect_device_build_sha256 is not None:
        if not re.fullmatch(r"[a-f0-9]{64}", arguments.expect_device_build_sha256):
            parser.error("--expect-device-build-sha256 requires 64 lowercase hexadecimal digits.")
        if arguments.runtime != "openxr":
            parser.error("--expect-device-build-sha256 requires --runtime openxr.")
    deadline = time.monotonic() + arguments.timeout
    host = android_deploy_host()
    runtime = {"requested": arguments.runtime, "status": "not_requested" if arguments.runtime == "none" else "not_observed"}
    if arguments.expect_runtime_version is not None:
        runtime["expectedVersion"] = arguments.expect_runtime_version
    connection = {"requested": arguments.connection, "observed": "not_observed"}
    report = {"schema": 1, "status": "partial", "stage": "selection",
              "qualification": {"host": {"requested": arguments.host, "observed": host},
                                "connection": connection, "runtime": runtime, "physicalXrQualification": "not_run"}}
    exit_code = 1
    launch = None
    bundle_admitted = False
    try:
        if arguments.diagnostic_bundle is not None:
            validate_deploy_bundle(arguments.diagnostic_bundle)
            bundle_admitted = True
        if arguments.host and arguments.host != host:
            raise AndroidDeployError("deployment_host_mismatch")
        if arguments.runtime == "openxr":
            if not arguments.host or not arguments.connection:
                raise AndroidDeployError("xr_target_selection_incomplete")
            if arguments.profile != OPENXR_DEVELOPMENT_PROFILE:
                report["stage"] = "artifact"
                android_profile(arguments.profile)
                report["stage"] = "runtime"
                report["qualification"]["profile"] = arguments.profile
                runtime["status"] = "unsupported"
                raise AndroidDeployError("android_openxr_route_unavailable")
        elif arguments.profile == OPENXR_DEVELOPMENT_PROFILE:
            raise AndroidDeployError("explicit_openxr_intent_required")
        report["stage"] = "artifact"
        with tempfile.TemporaryDirectory(prefix="horo-android-deploy-") as directory:
            artifact = deploy_artifact(
                arguments.artifact, Path(directory), arguments.sdk, arguments.profile, arguments.java, deadline)
            profile = artifact.profile
            if arguments.observe_seconds and profile["renderer"] != "vulkan-android":
                raise AndroidDeployError("observation_profile_unsupported")
            application = profile["applicationId"]
            report["artifact"] = {"profile": arguments.profile, "applicationId": application,
                                  "apkSha256": artifact.apk_sha256, "provenanceSha256": artifact.provenance_sha256,
                                  "sourceRevision": artifact.source_revision, "declaredRenderer": profile["renderer"],
                                  "abis": sorted(artifact.abis)}
            check_deployment_apk(report["artifact"], arguments.expect_apk_sha256)
            openxr = "openxrProfile" in profile
            if openxr:
                runtime["selection"] = "system-default"
                report["artifact"]["openxr"] = profile["openxrProfile"]
                report["qualification"]["profile"] = arguments.profile
            report["stage"] = "device"
            device = admit_adb_device(deploy_tool(arguments.sdk / "platform-tools", "adb"), arguments.device, deadline)
            connection["observed"] = device.connection
            if arguments.connection and device.connection == "unknown":
                raise AndroidDeployError("device_connection_unverifiable")
            if arguments.connection and arguments.connection != device.connection:
                raise AndroidDeployError("device_connection_mismatch")
            current_user, api, abi = deploy_device_identity(device, profile["minimumApi"], artifact.abis)
            report["device"] = {"androidApi": api, "abi": abi}
            if openxr:
                device_build = deploy_device_build_digest(device)
                report["device"]["buildSha256"] = device_build
                expected_build = arguments.expect_device_build_sha256
                if expected_build is not None:
                    report["device"]["expectedBuildSha256"] = expected_build
                    if expected_build != device_build:
                        raise AndroidDeployError("device_build_mismatch")
                report["artifact"]["openxrImages"] = select_openxr_device_images(artifact, abi)
            user = str(current_user)
            report["stage"] = "install"
            if device.process_id(application) is not None:
                raise AndroidDeployError("application_already_running")
            installed = device.run("install", "-r", "--user", user, str(artifact.path)).splitlines()
            if installed not in (["Success"], ["Performing Streamed Install", "Success"], ["Performing Push Install", "Success"]):
                raise AndroidDeployError("install_unconfirmed")
            report["stage"] = "verify_install"
            deploy_verified_install(device, application, user, artifact.apk_sha256)
            report["stage"] = "launch"
            launch = AndroidPackageLaunch(device, application, user, artifact.apk_sha256, openxr_diagnostic=openxr)
            process, window = launch.start()
            report["stage"] = "diagnostics"
            observed, applicable = await_deploy_diagnostics(device, application, process, window, profile, arguments.observe_seconds or 0)
            report["observed"] = observed
            if observed["crashObserved"]:
                raise AndroidDeployError("application_crashed")
            if openxr:
                check_openxr_discovery(runtime, observed["openxr"], abi)
                report["cleanup"] = launch.rollback()
                if report["cleanup"]["status"] not in ("stopped", "already_exited"):
                    raise AndroidDeployError("application_stop_unconfirmed")
            elif not applicable:
                if observed.get("observationWindow", {}).get("truncated"):
                    raise AndroidDeployError("observation_window_truncated")
                if arguments.observe_seconds and not observed["observationWindow"]["complete"]:
                    raise AndroidDeployError("observation_window_incomplete")
                raise AndroidDeployError("application_evidence_unavailable")
            deploy_verified_install(device, application, user, artifact.apk_sha256)
            if openxr and deploy_device_build_digest(device) != device_build:
                raise AndroidDeployError("device_build_changed")
            report["status"] = "diagnostic_complete" if openxr else "complete"
            exit_code = 0
    except KeyboardInterrupt:
        report["error"] = "cancelled"
        exit_code = 130
    except AndroidDeployError as error:
        report["error"] = error.code
    except (OSError, ValueError, KeyError, TypeError, AttributeError, zipfile.BadZipFile):
        report["error"] = "artifact_invalid" if report["stage"] == "artifact" else "host_operation_failed"
    if exit_code != 0 and launch is not None and launch.attempted and "cleanup" not in report:
        if launch.retired_crash:
            report["observed"] = {"crashObserved": True}
        if report["error"] == "device_transport_changed":
            report["cleanup"] = {"status": "unconfirmed", "reason": "device_transport_changed"}
        else:
            report["cleanup"] = launch.rollback()
    if bundle_admitted:
        try:
            write_deploy_bundle(arguments.diagnostic_bundle, report)
        except KeyboardInterrupt:
            report["bundleError"] = "cancelled"
            exit_code = 130
        except AndroidDeployError as error:
            report["bundleError"] = error.code
            if exit_code == 0:
                exit_code = 1
    print(canonical_json(report).decode("utf-8"), end="")
    return exit_code


# Bootstrap

#!/usr/bin/env python3


  # nosec B404 - the digest-verified SDK tool is the intended child.


MAXIMUM_ARCHIVE_BYTES = 256 * 1024 * 1024
MAXIMUM_EXPANDED_BYTES = 512 * 1024 * 1024
MAXIMUM_ENTRIES = 4096
PLATFORMS = ("linux-x64", "macos-arm64", "macos-x64", "windows-x64")
HOSTS = {
    "linux-x64": ("Linux", ("x86_64", "AMD64")),
    "macos-arm64": ("Darwin", ("arm64", "aarch64")),
    "macos-x64": ("Darwin", ("x86_64",)),
    "windows-x64": ("Windows", ("AMD64", "x86_64")),
}


def canonical_platform(value: str) -> str:
    # Return a literal from the supported matrix, never the caller's raw argv.
    if value == "linux-x64":
        return "linux-x64"
    if value == "macos-arm64":
        return "macos-arm64"
    if value == "macos-x64":
        return "macos-x64"
    if value == "windows-x64":
        return "windows-x64"
    raise ValueError("unsupported extension CI platform")


def canonical_commit(value: str) -> str:
    if len(value) != 40 or any(character not in "0123456789abcdef" for character in value):
        raise ValueError("invalid source commit attribution")
    # Pass a fresh, fixed-width hexadecimal value to the SDK runner, not raw CLI text.
    return f"{int(value, 16):040x}"


def canonical_repository(value: str) -> str:
    match = re.fullmatch(r"([A-Za-z0-9][A-Za-z0-9_.-]*)/([A-Za-z0-9_.-]+)", value)
    if match is None:
        raise ValueError("invalid repository attribution")
    owner, name = match.groups()
    # Reconstruct the bounded identifier rather than forwarding the raw CLI field.
    return f"{owner}/{name}"


def read_lock(project: Path, platform: str) -> tuple[str, str, str]:
    document = json.loads((project / ".horo/extension-ci.lock.json").read_text(encoding="utf-8"))
    if document.get("schemaVersion") != 1 or not isinstance(document.get("sdkVersion"), str):
        raise ValueError("invalid extension CI lock schema or SDK version")
    platforms = document.get("platforms")
    if platform not in PLATFORMS or not isinstance(platforms, dict) or set(platforms) != set(PLATFORMS):
        raise ValueError("extension CI lock must cover exactly the four supported platform/architecture tuples")
    entry = platforms[platform]
    if not isinstance(entry, dict):
        raise ValueError("extension CI lock platform entry must be an object")
    url, digest = entry.get("url"), entry.get("sha256")
    if not isinstance(url, str) or not url.startswith("https://") or "REPLACE_" in url:
        raise ValueError("pin the HTTPS SDK archive URL in .horo/extension-ci.lock.json")
    if not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{64}", digest) is None:
        raise ValueError("pin the lowercase SHA-256 SDK archive digest")
    return document["sdkVersion"], url, digest


def fetch_archive(url: str, digest: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "horo-extension-author-ci/1"})
    with urllib.request.urlopen(request, timeout=60) as response:  # nosec B310 - HTTPS and digest are required.
        if not response.geturl().startswith("https://"):
            raise ValueError("SDK download redirected outside HTTPS")
        data = response.read(MAXIMUM_ARCHIVE_BYTES + 1)
    if len(data) > MAXIMUM_ARCHIVE_BYTES:
        raise ValueError("SDK archive exceeds the bounded download size")
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError("SDK archive SHA-256 does not match the lock")
    return data


def safe_member_path(member: zipfile.ZipInfo, seen: set[str]) -> PurePosixPath:
    path = PurePosixPath(member.filename)
    mode = member.external_attr >> 16
    components = member.filename.rstrip("/").split("/")
    if (not member.filename or member.orig_filename != member.filename or "\\" in member.filename or path.is_absolute() or
            any(part in ("", ".", "..") for part in components) or
            ":" in components[0] or path.as_posix() != member.filename.rstrip("/") or
            (mode & 0o170000) not in (0, 0o100000, 0o040000) or member.filename.casefold() in seen):
        raise ValueError("SDK archive contains an unsafe or duplicate path")
    if member.is_dir() != ((mode & 0o170000) == 0o040000) and (mode & 0o170000) != 0:
        raise ValueError("SDK archive entry type disagrees with its path")
    seen.add(member.filename.casefold())
    return path


def extract_member(archive: zipfile.ZipFile, member: zipfile.ZipInfo, destination: Path,
                   path: PurePosixPath) -> None:
    target = destination.joinpath(*path.parts)
    if member.is_dir():
        target.mkdir(parents=True, exist_ok=True)
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    with archive.open(member) as source, target.open("xb") as output:
        while block := source.read(1024 * 1024):
            output.write(block)
    mode = member.external_attr >> 16
    if os.name != "nt" and (mode & 0o170000) == 0o100000:
        target.chmod(mode & 0o777)


def extract_sdk(data: bytes, destination: Path) -> None:
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        members = archive.infolist()
        if len(members) > MAXIMUM_ENTRIES or sum(member.file_size for member in members) > MAXIMUM_EXPANDED_BYTES:
            raise ValueError("SDK archive exceeds bounded extraction limits")
        seen: set[str] = set()
        for member in members:
            path = safe_member_path(member, seen)
            extract_member(archive, member, destination, path)


def author_paths(args: argparse.Namespace) -> tuple[Path, Path]:
    project = args.project.resolve(strict=True)
    if not project.is_dir():
        raise ValueError("author project must be a directory")
    lock = project / ".horo/extension-ci.lock.json"
    output = project / "ci-artifacts"
    if args.lock.absolute() != lock or lock.resolve(strict=True) != lock:
        raise ValueError("extension CI lock must be the author project's .horo lock")
    if args.output.absolute() != output or output.is_symlink():
        raise ValueError("extension CI output must be the author project's ci-artifacts")
    if re.fullmatch(r"[0-9a-f]{40}", args.commit) is None or \
            re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository) is None:
        raise ValueError("invalid source commit or repository attribution")
    return project, output


def bootstrap_main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, required=True)
    parser.add_argument("--platform", choices=PLATFORMS, required=True)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--commit", required=True)
    args = parser.parse_args(argv)
    try:
        platform = canonical_platform(args.platform)
        operating_system, machines = HOSTS[platform]
        if host_platform.system() != operating_system or host_platform.machine() not in machines:
            raise ValueError("CI runner architecture does not match the locked platform")
        project, output = author_paths(args)
        commit = canonical_commit(args.commit)
        repository = canonical_repository(args.repository)
        version, url, digest = read_lock(project, platform)
        with tempfile.TemporaryDirectory(prefix="horo-extension-sdk-") as temporary:
            root = Path(temporary)
            extract_sdk(fetch_archive(url, digest), root)
            metadata = json.loads((root / "share/horo/extension-sdk/extension-sdk.json").read_text(encoding="utf-8"))
            if metadata["sdk"]["version"] != version:
                raise ValueError("downloaded SDK version differs from the lock")
            runner = root / "bin/project.py"
            if not runner.is_file() or runner.is_symlink():
                raise ValueError("downloaded SDK does not contain the author CI tool")
            command = [sys.executable, str(runner), "extension-ci", "--sdk", str(root), "--project", str(project),
                       "--output", str(output), "--platform", platform,
                       "--repository", repository, "--commit", commit,
                       "--sdk-sha256", digest]
            # The runner came from the SHA-256-pinned archive, arguments are separate
            # argv values, and no user-provided text is interpreted by a shell.
            # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-tainted-env-args.dangerous-subprocess-use-tainted-env-args,python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit
            return subprocess.run(command, check=False, shell=False).returncode  # nosec B603
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print(f"extension CI bootstrap: {error}", file=sys.stderr)
        return 2


# Author

#!/usr/bin/env python3


  # nosec B404 - fixed SDK and CMake tools are the contract under test.


# The bounded parser below rejects DTD/entities and disables parameter entities.
# nosemgrep: python.lang.security.use-defused-xml.use-defused-xml


MAXIMUM_JUNIT_BYTES = 8 * 1024 * 1024
SDK_TOOLS = frozenset(("horo-extension-validate", "horo-extension-conformance", "horo-package"))
MANIFEST_NAME = "extension.json"
JUNIT_NAME = "ctest.xml"
ARCHIVE_NAME = "extension.horopkg"
PROVENANCE_NAME = "provenance.json"


def author_run(sdk: Path, tool: str, *arguments: str) -> None:
    if tool in ("cmake", "ctest"):
        executable = tool
    elif tool in SDK_TOOLS:
        executable = sdk_tool(sdk, tool)
    else:
        raise ValueError(f"unsupported extension CI tool: {tool}")
    # Only named tools are executable. Project-derived values remain separate
    # argv elements, never shell text; SDK tools came from the digest-checked ZIP.
    # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-tainted-env-args.dangerous-subprocess-use-tainted-env-args,python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit
    subprocess.run((executable, *arguments), check=True, shell=False)  # nosec B603


def sdk_tool(root: Path, name: str) -> str:
    if name not in SDK_TOOLS:
        raise ValueError(f"unsupported extension SDK tool: {name}")
    suffix = ".exe" if os.name == "nt" else ""
    tool = root / "bin" / (name + suffix)
    if not tool.is_file() or tool.is_symlink():
        raise ValueError(f"required SDK tool is missing: {name}")
    return str(tool)


def module_paths(stage: Path) -> tuple[Path, ...]:
    manifest = stage / MANIFEST_NAME
    document = json.loads(manifest.read_text(encoding="utf-8"))
    package_id = document.get("id")
    modules = document.get("modules")
    if not isinstance(package_id, str) or not package_id or not isinstance(modules, list) or not modules:
        raise ValueError("installed extension manifest has no package identity or native modules")
    binaries: list[Path] = []
    for module in modules:
        entry = module.get("entry") if isinstance(module, dict) else None
        if not isinstance(entry, str) or "\\" in entry or PurePosixPath(entry).parts[:1] != ("bin",) or \
                len(PurePosixPath(entry).parts) != 2 or entry != PurePosixPath(entry).as_posix():
            raise ValueError("installed extension manifest has an unsafe module entry")
        binary = stage / entry
        if not binary.is_file() or binary.is_symlink():
            raise ValueError(f"installed extension module is absent: {entry}")
        binaries.append(binary)
    return tuple(binaries)


def package_identity(stage: Path) -> str:
    # Only the Python 3.11+ extension author runner consumes TOML.
    import tomllib

    manifest = json.loads((stage / MANIFEST_NAME).read_text(encoding="utf-8"))
    with (stage / "horo-package.toml").open("rb") as source:
        package = tomllib.load(source)
    identity = package.get("package", {})
    if (package.get("schemaVersion") != 1 or not isinstance(identity, dict) or
            identity.get("id") != manifest.get("id") or not isinstance(identity.get("version"), str) or
            any(module.get("version") != identity["version"] for module in manifest.get("modules", []))):
        raise ValueError("installed package and extension manifest identities disagree")
    return identity["id"]


def require_executed_tests(path: Path) -> None:
    with path.open("rb") as source:
        report_bytes = source.read(MAXIMUM_JUNIT_BYTES + 1)
    if len(report_bytes) > MAXIMUM_JUNIT_BYTES:
        raise ValueError("CTest JUnit report exceeds the bounded parse size")
    try:
        from defusedxml import ElementTree
        from defusedxml.common import DefusedXmlException
    except ImportError as error:
        raise ValueError("CTest JUnit validation requires defusedxml; install the SDK author dependencies") from error
    case_open, skipped, executed = False, False, 0
    try:
        events = ElementTree.iterparse(io.BytesIO(report_bytes), events=("start", "end"),
                                       forbid_dtd=True, forbid_entities=True, forbid_external=True)
        for event, element in events:
            if event == "start" and element.tag == "testcase":
                if case_open:
                    raise ValueError("CTest JUnit report contains nested testcases")
                case_open, skipped = True, False
            elif event == "start" and element.tag == "skipped" and case_open:
                skipped = True
            elif event == "end":
                if element.tag == "testcase":
                    if not skipped:
                        executed += 1
                    case_open = False
                element.clear()
    except (ElementTree.ParseError, DefusedXmlException) as error:
        raise ValueError("CTest JUnit report is malformed or contains forbidden DTD or entity declarations") from error
    if not executed:
        raise ValueError("CTest reported no executed extension tests")


def build_and_stage(sdk: Path, project: Path, scratch: Path) -> Path:
    build, stage = scratch / "build", scratch / "stage"
    author_run(sdk, "cmake", "-S", str(project), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
        f"-DHoroEngineExtensionSdk_DIR={sdk / 'lib/cmake/HoroEngineExtensionSdk'}")
    author_run(sdk, "cmake", "--build", str(build), "--config", "Release", "--parallel", "2")
    author_run(sdk, "ctest", "--test-dir", str(build), "-C", "Release", "--output-on-failure",
        "--output-junit", str(scratch / JUNIT_NAME))
    require_executed_tests(scratch / JUNIT_NAME)
    author_run(sdk, "cmake", "--install", str(build), "--config", "Release", "--prefix", str(stage))
    return stage


def package_and_publish(sdk: Path, stage: Path, scratch: Path, output: Path, args: argparse.Namespace,
                        sdk_version: str) -> None:
    manifest = stage / MANIFEST_NAME
    author_run(sdk, "horo-extension-validate", "--json", "--schema-version", "1", str(manifest))
    for binary in module_paths(stage):
        author_run(sdk, "horo-extension-conformance", "--json", str(binary))
    if not (stage / "horo-package.toml").is_file():
        raise ValueError("project must install horo-package.toml for canonical packaging")
    package_id = package_identity(stage)
    archive = scratch / ARCHIVE_NAME
    author_run(sdk, "horo-package", "pack", str(stage), str(archive))
    trust = scratch / "ci-integrity-only-trust.json"
    trust.write_text('{"schemaVersion":1,"allowUnsigned":true,"publishers":[]}', encoding="utf-8")
    author_run(sdk, "horo-package", "verify", str(archive), "--package-id", package_id, "--trust", str(trust))
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    output.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(archive, output / ARCHIVE_NAME)
    shutil.copyfile(scratch / JUNIT_NAME, output / JUNIT_NAME)
    provenance = {
        "schemaVersion": 1,
        "repository": args.repository,
        "commit": args.commit,
        "platform": args.platform,
        "sdkVersion": sdk_version,
        "sdkSha256": args.sdk_sha256,
        "packageId": package_id,
        "artifact": ARCHIVE_NAME,
        "artifactSha256": digest,
        "verification": "archive-integrity-unsigned",
    }
    (output / PROVENANCE_NAME).write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8")


def author_main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--platform", choices=("linux-x64", "macos-arm64", "macos-x64", "windows-x64"), required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--sdk-sha256", required=True)
    args = parser.parse_args(argv)
    try:
        if re.fullmatch(r"[0-9a-f]{40}", args.commit) is None or \
                re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository) is None or \
                re.fullmatch(r"[0-9a-f]{64}", args.sdk_sha256) is None:
            raise ValueError("invalid commit, repository or SDK digest attribution")
        sdk = args.sdk.resolve(strict=True)
        project = args.project.resolve(strict=True)
        output = project / "ci-artifacts"
        if args.output.absolute() != output:
            raise ValueError("artifact output must be the author project's ci-artifacts")
        if output.is_symlink():
            raise ValueError("artifact output cannot be a symlink")
        if output.exists() and (not output.is_dir() or any(output.iterdir())):
            raise ValueError("artifact output must be absent or empty")
        metadata = json.loads((sdk / "share/horo/extension-sdk/extension-sdk.json").read_text(encoding="utf-8"))
        sdk_version = metadata["sdk"]["version"]
        with tempfile.TemporaryDirectory(prefix="horo-extension-author-ci-") as temporary:
            scratch = Path(temporary)
            stage = build_and_stage(sdk, project, scratch)
            package_and_publish(sdk, stage, scratch, output, args, sdk_version)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"extension author CI: {error}", file=sys.stderr)
        return 2
    return 0


# Scaffold

#!/usr/bin/env python3


MAXIMUM_IDENTIFIER_BYTES = 256
MAXIMUM_SEMANTIC_VERSION_BYTES = 64
MAXIMUM_DISPLAY_NAME_BYTES = 4 * 1024
MAXIMUM_PORTABLE_FILENAME_BYTES = 255
LONGEST_SHARED_MODULE_SUFFIX = ".dylib"
LONGEST_EXECUTABLE_SUFFIX = ".exe"


@dataclass(frozen=True)
class Module:
    suffix: str
    roles: tuple[str, ...]
    exports_service: bool = False
    imports_backend: bool = False


SHAPES = {
    "gui": (Module("editor", ("editor-presentation",)),),
    "backend": (Module("backend", ("backend-capability", "headless-tooling"), True),),
    "script": (Module("script", ("backend-capability", "script-provider"), True),),
    "hybrid": (
        Module("backend", ("backend-capability", "headless-tooling"), True),
        Module("editor", ("editor-presentation",), imports_backend=True),
        Module("script", ("script-provider",), imports_backend=True),
    ),
}


MODULE_SOURCE = r'''#include "Horo/Extensions/ExtensionAbi.h"

#include <stddef.h>

static const char module_id[] = "@MODULE_ID@";
static const char module_version[] = "@VERSION@";

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_query(HoroExtensionRequirements *requirements) {
    if (requirements == NULL || requirements->structSize < sizeof(*requirements))
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    *requirements = (HoroExtensionRequirements){
        .structSize = sizeof(*requirements),
        .abiMajorVersion = HORO_EXTENSION_ABI_VERSION,
        .minimumHostMinor = HORO_EXTENSION_ABI_MINOR_VERSION,
        .requiredHostApiSize = sizeof(HoroExtensionHostApi),
    };
    return HORO_EXTENSION_SUCCESS;
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *host, HoroExtensionModuleApi *module) {
    if (host == NULL || module == NULL || module->structSize < sizeof(*module) ||
        host->structSize < sizeof(*host) || host->abiVersion != HORO_EXTENSION_ABI_VERSION)
        return HORO_EXTENSION_ERROR_VERSION_MISMATCH;
    *module = (HoroExtensionModuleApi){
        .structSize = sizeof(*module),
        .moduleId = {module_id, sizeof(module_id) - 1},
        .moduleVersion = {module_version, sizeof(module_version) - 1},
    };
    return HORO_EXTENSION_SUCCESS;
}

HORO_EXTENSION_EXPORT void horo_extension_unload(HoroExtensionModuleApi *module) {
    if (module != NULL)
        *module = (HoroExtensionModuleApi){0};
}
'''


CONTRACT_TEST = r'''#include "Horo/Extensions/ExtensionAbi.h"

#include <stddef.h>
#include <string.h>

HoroExtensionStatus horo_extension_query(HoroExtensionRequirements *requirements);
HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *host, HoroExtensionModuleApi *module);
void horo_extension_unload(HoroExtensionModuleApi *module);

int main(void) {
    static const char expected_id[] = "@MODULE_ID@";
    HoroExtensionRequirements requirements = {.structSize = sizeof(requirements)};
    if (horo_extension_query(&requirements) != HORO_EXTENSION_SUCCESS ||
        requirements.abiMajorVersion != HORO_EXTENSION_ABI_VERSION ||
        requirements.minimumHostMinor > HORO_EXTENSION_ABI_MINOR_VERSION)
        return 1;
    const HoroExtensionHostApi host = {
        .structSize = sizeof(host),
        .abiVersion = HORO_EXTENSION_ABI_VERSION,
        .abiMinorVersion = HORO_EXTENSION_ABI_MINOR_VERSION,
    };
    HoroExtensionModuleApi module = {.structSize = sizeof(module)};
    if (horo_extension_load(&host, &module) != HORO_EXTENSION_SUCCESS ||
        module.moduleId.length != sizeof(expected_id) - 1 ||
        memcmp(module.moduleId.data, expected_id, sizeof(expected_id) - 1) != 0)
        return 2;
    horo_extension_unload(&module);
    return module.moduleContext == NULL ? 0 : 3;
}
'''


def module_id(package_id: str, module: Module) -> str:
    return f"{package_id}.{module.suffix}"


def target_name(package_id: str, module: Module) -> str:
    return f"{re.sub(r'[^A-Za-z0-9]', '_', package_id)}_{module.suffix}"


def derived_identifiers(package_id: str, modules: tuple[Module, ...]) -> tuple[str, ...]:
    service_id = f"{package_id}.service"
    backend_id = f"{package_id}.backend"
    identifiers = {package_id}
    for module in modules:
        current_module_id = module_id(package_id, module)
        identifiers.add(current_module_id)
        if module.exports_service:
            identifiers.update((service_id,))
        if module.imports_backend:
            identifiers.update((backend_id, f"{current_module_id}.backend", service_id))
    return tuple(sorted(identifiers))


def generated_filenames(package_id: str, version: str, modules: tuple[Module, ...]) -> tuple[str, ...]:
    filenames = [f"{package_id}-{version}.zip"]
    for module in modules:
        target = target_name(package_id, module)
        filenames.extend((target + LONGEST_SHARED_MODULE_SUFFIX, target + "_contract_test" + LONGEST_EXECUTABLE_SUFFIX))
    return tuple(filenames)


def is_ascii_lower(character: str) -> bool:
    return "a" <= character <= "z"


def is_ascii_digit(character: str) -> bool:
    return "0" <= character <= "9"


def is_canonical_identifier(value: str) -> bool:
    if not value or len(value.encode("utf-8")) > MAXIMUM_IDENTIFIER_BYTES:
        return False
    for segment in value.split("."):
        if not segment or not is_ascii_lower(segment[0]) or segment[-1] == "-":
            return False
        if not all(is_ascii_lower(character) or is_ascii_digit(character) or character == "-" for character in segment):
            return False
    return True


def is_canonical_numeric_component(value: str) -> bool:
    return bool(value) and not (len(value) > 1 and value[0] == "0") and all(is_ascii_digit(character) for character in value)


def is_canonical_semantic_version(value: str) -> bool:
    if not value or len(value.encode("utf-8")) > MAXIMUM_SEMANTIC_VERSION_BYTES or "+" in value:
        return False
    core, separator, prerelease = value.partition("-")
    if len(core.split(".")) != 3 or not all(is_canonical_numeric_component(part) for part in core.split(".")):
        return False
    if not separator:
        return True
    for identifier in prerelease.split("."):
        if not identifier or not all(character.isascii() and (character.isalnum() or character == "-") for character in identifier):
            return False
        if all(is_ascii_digit(character) for character in identifier) and len(identifier) > 1 and identifier[0] == "0":
            return False
    return True


def render(text: str, replacements: dict[str, str]) -> str:
    for key, value in replacements.items():
        text = text.replace(f"@{key}@", value)
    return text


def scaffold_manifest(package_id: str, name: str, version: str, modules: tuple[Module, ...]) -> dict:
    service_id = f"{package_id}.service"
    backend_id = f"{package_id}.backend"
    module_values = []
    for module in modules:
        value = {
            "id": module_id(package_id, module),
            "version": version,
            "kind": "native",
            "entry": f"bin/{target_name(package_id, module)}@CMAKE_SHARED_MODULE_SUFFIX@",
            "roles": list(module.roles),
            "abi": {"major": 1, "minimumMinor": 1},
        }
        if module.exports_service:
            value["exports"] = [{"id": service_id, "contract": service_id, "version": version}]
        if module.imports_backend:
            value["dependencies"] = [backend_id]
            value["imports"] = [{
                "id": f"{module_id(package_id, module)}.backend",
                "service": service_id,
                "contract": service_id,
                "minimumVersion": version,
            }]
        module_values.append(value)
    return {
        "schemaVersion": 1,
        "id": package_id,
        "displayName": name,
        "version": version,
        "modules": module_values,
        "contributions": [],
    }


def cmake_project(package_id: str, version: str, modules: tuple[Module, ...]) -> str:
    lines = [
        "cmake_minimum_required(VERSION 3.25)",
        "project(HoroExtension LANGUAGES C)",
        "find_package(HoroEngineExtensionSdk CONFIG REQUIRED)",
        "include(CTest)",
        'configure_file(extension.json.in "${CMAKE_CURRENT_BINARY_DIR}/extension.json" @ONLY)',
        "",
    ]
    for module in modules:
        target = target_name(package_id, module)
        source = f"src/{module.suffix}.c"
        test_target = f"{target}_contract_test"
        lines.extend([
            f"add_library({target} MODULE {source})",
            f"target_compile_features({target} PRIVATE c_std_11)",
            f"target_link_libraries({target} PRIVATE HoroEngine::ExtensionSdk)",
            f"set_target_properties({target} PROPERTIES PREFIX \"\" C_EXTENSIONS OFF C_VISIBILITY_PRESET hidden)",
            f"add_executable({test_target} tests/{module.suffix}_contract.c {source})",
            f"target_compile_features({test_target} PRIVATE c_std_11)",
            f"target_link_libraries({test_target} PRIVATE HoroEngine::ExtensionSdk)",
            f"set_target_properties({test_target} PROPERTIES C_EXTENSIONS OFF)",
            f"add_test(NAME {test_target} COMMAND {test_target})",
            f"install(TARGETS {target} DESTINATION bin)",
            "",
        ])
    lines.extend([
        'install(FILES "${CMAKE_CURRENT_BINARY_DIR}/extension.json" DESTINATION .)',
        'install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/horo-package.toml" DESTINATION .)',
        f'set(CPACK_PACKAGE_NAME "{package_id}")',
        f'set(CPACK_PACKAGE_VERSION "{version}")',
        f'set(CPACK_PACKAGE_FILE_NAME "{package_id}-{version}")',
        'set(CPACK_GENERATOR "ZIP")',
        "set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)",
        "include(CPack)",
        "",
    ])
    return "\n".join(lines)


def ci_template_dir() -> Path:
    root = Path(__file__).resolve().parent.parent
    packaged = root / "share/horo/extension-sdk/ci"
    if packaged.is_dir():
        return packaged
    return root / "sdk/ci"


def scaffold_write_project(root: Path, package_id: str, name: str, version: str, shape: str) -> None:
    modules = SHAPES[shape]
    (root / "src").mkdir(parents=True)
    (root / "tests").mkdir()
    (root / ".github/workflows").mkdir(parents=True)
    (root / ".horo/ci").mkdir(parents=True)
    # Every destination below is a fixed or closed-table child of the private staging root.
    (root / "CMakeLists.txt").write_text(  # NOSONAR
        cmake_project(package_id, version, modules), encoding="utf-8"
    )
    (root / "extension.json.in").write_text(  # NOSONAR
        json.dumps(scaffold_manifest(package_id, name, version, modules), indent=2) + "\n", encoding="utf-8"
    )
    (root / "horo-package.toml").write_text(  # NOSONAR
        "schemaVersion = 1\n\n[package]\n"
        f"id = {json.dumps(package_id, ensure_ascii=False)}\n"
        f"version = {json.dumps(version, ensure_ascii=False)}\n"
        "kind = \"extension\"\n"
        f"displayName = {json.dumps(name, ensure_ascii=False)}\n",
        encoding="utf-8",
    )
    templates = ci_template_dir()
    lock_file = templates / "extension-ci.lock.json"
    if not lock_file.is_file():
        lock_file = templates / "extension-ci.lock.json.in"
    shutil.copyfile(templates / "extension-author-ci.yml", root / ".github/workflows/extension-ci.yml")
    shutil.copyfile(Path(__file__), root / ".horo/ci/project.py")
    shutil.copyfile(lock_file, root / ".horo/extension-ci.lock.json")
    for module in modules:
        replacements = {"MODULE_ID": module_id(package_id, module), "VERSION": version}
        (root / "src" / f"{module.suffix}.c").write_text(  # NOSONAR
            render(MODULE_SOURCE, replacements), encoding="utf-8"
        )
        (root / "tests" / f"{module.suffix}_contract.c").write_text(  # NOSONAR
            render(CONTRACT_TEST, replacements), encoding="utf-8"
        )
    (root / "README.md").write_text(  # NOSONAR
        f"# {name}\n\nGenerated `{shape}` Horo extension scaffold.\n\n"
        "Configure with `HoroEngineExtensionSdk_DIR` pointing to the SDK's "
        "`lib/cmake/HoroEngineExtensionSdk` directory. Build, run CTest, then build "
        "the `package` target to create the ZIP artifact.\n\n"
        "For source-free author CI, edit `.horo/extension-ci.lock.json` with the "
        "version-matched HTTPS SDK ZIP URL and SHA-256 for each platform. The "
        "pinned `.github/workflows/extension-ci.yml` builds, validates, tests, "
        "packages and verifies the unsigned archive on Linux, macOS and Windows. "
        "It publishes an attributable `.horopkg` and provenance, but does not "
        "sign releases or establish publisher trust.\n",
        encoding="utf-8",
    )


def validate(args: argparse.Namespace) -> None:
    if not is_canonical_identifier(args.id):
        raise ValueError("--id must be a lowercase reverse-domain identifier")
    if not is_canonical_semantic_version(args.version):
        raise ValueError("--version must be a canonical semantic version")
    if len(args.name.strip().encode("utf-8")) > MAXIMUM_DISPLAY_NAME_BYTES:
        raise ValueError("--name exceeds the manifest display-name limit")
    modules = SHAPES[args.shape]
    if not all(is_canonical_identifier(identifier) for identifier in derived_identifiers(args.id, modules)):
        raise ValueError("--id is too long for identifiers derived by this scaffold shape")
    if not all(len(filename.encode("utf-8")) <= MAXIMUM_PORTABLE_FILENAME_BYTES
               for filename in generated_filenames(args.id, args.version, modules)):
        raise ValueError("--id and --version produce a filename longer than the portable component limit")
    if os.path.lexists(args.output):
        if args.output.is_symlink() or not args.output.is_dir() or any(args.output.iterdir()):
            raise ValueError("--output must be an absent path or empty directory")


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shape", choices=tuple(SHAPES), required=True)
    parser.add_argument("--id", required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--version", default="1.0.0")
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args(argv)


def scaffold_main(argv=None) -> int:
    args = parse_args(argv)
    try:
        validate(args)
        output = args.output.absolute()
        output.parent.mkdir(parents=True, exist_ok=True)
        output_parent = output.parent.resolve(strict=True)
        if output.name in {"", ".", ".."}:
            raise ValueError("--output must name a project directory")
        output = output_parent / output.name
        staging: Path | None = Path(tempfile.mkdtemp(prefix=".horo-extension.", dir=output_parent))
        try:
            scaffold_write_project(staging, args.id, args.name.strip(), args.version, args.shape)
            if output.exists():
                output.rmdir()
            staging.replace(output)
            staging = None
        finally:
            if staging is not None:
                shutil.rmtree(staging, ignore_errors=True)
    except (OSError, ValueError) as error:
        print(f"error: {error}")
        return 2
    return 0


# Gameplay

#!/usr/bin/env python3


MAXIMUM_SOURCE_FILES = 4096
MAXIMUM_BEHAVIORS = 4096
MAXIMUM_SOURCE_BYTES = 4 * 1024 * 1024
MAXIMUM_TOTAL_SOURCE_BYTES = 32 * 1024 * 1024
MAXIMUM_IDENTITY_BYTES = 256
MODULE_ID = re.compile(r"[a-z][a-z0-9_]*(?:\.[a-z0-9_]+)+\Z")
FINGERPRINT = re.compile(r"[A-Za-z0-9_.-]+\Z")
TYPE_ID = re.compile(r"game\.[a-z0-9_]+(?:\.[a-z0-9_]+)+\Z")
TOKEN = re.compile(
    r"(?P<space>\s+)|(?P<line_comment>//[^\n]*)|(?P<block_comment>/\*.*?\*/)|"
    r"(?P<string>\"(?:\\.|[^\"\\])*\")|(?P<identifier>[A-Za-z_]\w*)|"
    r"(?P<punctuation>[(),])|(?P<other>.)",
    re.DOTALL,
)


def parse(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--revision-output", required=True, type=pathlib.Path)
    parser.add_argument("--output-root", required=True, type=pathlib.Path)
    parser.add_argument("--module-id", required=True)
    parser.add_argument("--fingerprint", required=True)
    parser.add_argument("sources", nargs="+", type=pathlib.Path)
    return parser.parse_args(argv)


def significant_tokens(text: str) -> list[tuple[str, str]]:
    ignored = {"space", "line_comment", "block_comment"}
    return [(match.lastgroup or "other", match.group()) for match in TOKEN.finditer(text) if match.lastgroup not in ignored]


def annotations_in(text: str, source: pathlib.Path, marker: str = "HORO_BEHAVIOR") -> list[tuple[str, str]]:
    tokens = significant_tokens(text)
    annotations: list[tuple[str, str]] = []
    expected = ("identifier", "punctuation", "identifier", "punctuation", "string", "punctuation")
    for index, token in enumerate(tokens):
        if token != ("identifier", marker):
            continue
        candidate = tokens[index : index + len(expected)]
        if len(candidate) != len(expected) or tuple(kind for kind, _ in candidate) != expected:
            raise ValueError(f"malformed {marker} annotation in {source}")
        _, opening, symbol, comma, encoded_type_id, closing = candidate
        if opening[1] != "(" or comma[1] != "," or closing[1] != ")":
            raise ValueError(f"malformed {marker} annotation in {source}")
        type_id = json.loads(encoded_type_id[1])
        annotations.append((symbol[1], type_id))
    return annotations


def read_sources(sources: list[pathlib.Path]) -> list[tuple[pathlib.Path, str, str]]:
    if len(sources) > MAXIMUM_SOURCE_FILES:
        raise ValueError("gameplay descriptor source count exceeds the supported limit")
    resolved = sorted(source.resolve() for source in sources)
    if len(set(resolved)) != len(resolved):
        raise ValueError("gameplay descriptor sources contain duplicate paths")
    records: list[tuple[pathlib.Path, str, str]] = []
    total_bytes = 0
    for source in resolved:
        if not source.is_file():
            raise ValueError(f"source path does not exist or is not a regular file: {source}")
        encoded = source.read_bytes()
        total_bytes += len(encoded)
        if len(encoded) > MAXIMUM_SOURCE_BYTES or total_bytes > MAXIMUM_TOTAL_SOURCE_BYTES:
            raise ValueError("gameplay descriptor source bytes exceed the supported limit")
        records.append((source, encoded.decode("utf-8"), hashlib.sha256(encoded).hexdigest()))
    return records


def validate_annotations(annotations: list[tuple[str, str, pathlib.Path]]) -> None:
    if len(annotations) > MAXIMUM_BEHAVIORS:
        raise ValueError("native behavior annotation count exceeds the supported limit")
    seen_symbols: set[str] = set()
    seen_ids: set[str] = set()
    for symbol, type_id, source in annotations:
        if symbol in seen_symbols:
            raise ValueError(f"duplicate native behavior symbol {symbol!r} in {source}")
        if type_id in seen_ids:
            raise ValueError(f"duplicate native behavior type ID {type_id!r} in {source}")
        if not TYPE_ID.fullmatch(type_id):
            raise ValueError(f"invalid native behavior type ID {type_id!r} in {source}")
        seen_symbols.add(symbol)
        seen_ids.add(type_id)


def descriptor_revision(module_id: str, annotations: list[tuple[str, str, pathlib.Path]], source_hashes: list[str]) -> int:
    identity = ["horo.generated-gameplay-descriptor.v1", module_id]
    identity.extend(f"{type_id}\0{symbol}" for symbol, type_id, _ in annotations)
    identity.extend(sorted(source_hashes))
    revision = int.from_bytes(hashlib.sha256("\n".join(identity).encode("utf-8")).digest()[:8], "big")
    return revision or 1


def confined_output(root: pathlib.Path, path: pathlib.Path) -> pathlib.Path:
    canonical_root = root.resolve(strict=True)
    canonical_path = path.resolve(strict=False)
    if not canonical_path.is_relative_to(canonical_root):
        raise ValueError(f"generated output must remain inside {canonical_root}")
    return canonical_path


def write_atomic(path: pathlib.Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(content, encoding="utf-8")
    temporary.replace(path)


def generated_storage(annotations: list[tuple[str, str, pathlib.Path]]) -> str:
    declarations = "\n".join(
        f"Horo::Gameplay::BehaviorDescriptor HoroGeneratedBehaviorDescriptor_{symbol}();\n"
        f"Horo::Gameplay::BehaviorFactoryBinding HoroGeneratedBehaviorFactory_{symbol}();"
        for symbol, _, _ in annotations
    )
    descriptors = ",\n        ".join(f"HoroGeneratedBehaviorDescriptor_{symbol}()" for symbol, _, _ in annotations)
    factories = ",\n        ".join(
        "{.typeId = Horo::Gameplay::BehaviorTypeId::Parse(" + json.dumps(type_id) + ").Value(), "
        f".factory = HoroGeneratedBehaviorFactory_{symbol}()}}"
        for symbol, type_id, _ in annotations
    )
    count = len(annotations)
    return f'''// Generated by scripts/project.py gameplay. Do not edit.
#include "Horo/Gameplay/GameModule.h"
#include <array>

{declarations}

extern "C" HORO_GAME_EXPORT Horo::Gameplay::IGameModule *CreateGameModule() noexcept;
extern "C" HORO_GAME_EXPORT void DestroyGameModule(Horo::Gameplay::IGameModule *) noexcept;

namespace {{
const auto &Descriptors() {{
    static const std::array<Horo::Gameplay::BehaviorDescriptor, {count}> descriptors{{{{
        {descriptors}
    }}}};
    return descriptors;
}}

const auto &Factories() {{
    static const std::array<Horo::Gameplay::GeneratedBehaviorFactoryBinding, {count}> factories{{{{
        {factories}
    }}}};
    return factories;
}}
}}
'''


def generated_exports(module_id: str, fingerprint: str, revision: int, count: int, node_count: int = 0, sense_count: int = 0) -> str:
    behavior_pointer = "Descriptors().data()" if count else "nullptr"
    factory_pointer = "Factories().data()" if count else "nullptr"
    node_pointer = "DecisionNodes().data()" if node_count else "nullptr"
    sense_pointer = "PerceptionSenses().data()" if sense_count else "nullptr"
    return f'''

extern "C" HORO_GAME_EXPORT const Horo::Gameplay::GameModuleDescriptor *GetGameModuleDescriptor() noexcept {{
    static const Horo::Gameplay::GameModuleDescriptor descriptor{{
        .structSize = sizeof(Horo::Gameplay::GameModuleDescriptor),
        .sdkBoundaryVersion = Horo::Gameplay::GameplaySdkBoundaryVersion,
        .moduleId = {json.dumps(module_id)},
        .buildFingerprint = {json.dumps(fingerprint)},
    }};
    return &descriptor;
}}

extern "C" HORO_GAME_EXPORT const Horo::Gameplay::GeneratedGameplayDescriptorBundle *GetGameplayDescriptorBundle() noexcept {{
    static const Horo::Gameplay::GeneratedGameplayDescriptorBundle bundle{{
        .structSize = sizeof(Horo::Gameplay::GeneratedGameplayDescriptorBundle),
        .schemaVersion = Horo::Gameplay::GameplayDescriptorBundleSchemaVersion,
        .sdkBoundaryVersion = Horo::Gameplay::GameplaySdkBoundaryVersion,
        .moduleId = {json.dumps(module_id)},
        .buildFingerprint = {json.dumps(fingerprint)},
        .descriptorRevision = {revision}ULL,
        .behaviors = {behavior_pointer},
        .behaviorCount = {count},
        .nativeFactoryBindings = {factory_pointer},
        .nativeFactoryBindingCount = {count},
        // Blocking scanner diagnostics abort generation before a bundle is published.
        .diagnostics = nullptr,
        .diagnosticCount = 0,
        .lifecycle = {{.create = &CreateGameModule, .destroy = &DestroyGameModule}},
        .decisionNodes = {node_pointer},
        .decisionNodeCount = {node_count},
        .perceptionSenses = {sense_pointer},
        .perceptionSenseCount = {sense_count},
    }};
    return &bundle;
}}
'''


def generated_source(
    module_id: str,
    fingerprint: str,
    revision: int,
    annotations: list[tuple[str, str, pathlib.Path]],
    nodes: list[tuple[str, str, pathlib.Path]] | None = None,
    senses: list[tuple[str, str, pathlib.Path]] | None = None,
) -> str:
    nodes = nodes or []
    senses = senses or []
    return (generated_storage(annotations) + generated_node_storage(nodes) + generated_sense_storage(senses)
            + generated_exports(module_id, fingerprint, revision, len(annotations), len(nodes), len(senses)))


def generated_node_storage(nodes: list[tuple[str, str, pathlib.Path]]) -> str:
    declarations = "\n".join(
        f"Horo::AI::DecisionNodeRegistration HoroGeneratedDecisionNode_{symbol}();"
        for symbol, _, _ in nodes
    )
    records = ",\n        ".join(f"HoroGeneratedDecisionNode_{symbol}()" for symbol, _, _ in nodes)
    return f'''
{declarations}
namespace {{
const auto &DecisionNodes() {{
    static const std::array<Horo::AI::DecisionNodeRegistration, {len(nodes)}> nodes{{{{
        {records}
    }}}};
    return nodes;
}}
}}
'''


def native_ai_annotations(sources: list[tuple[pathlib.Path, str, str]], marker: str, kind: str,
                          maximum: int, minimum: int) -> list[tuple[str, str, pathlib.Path]]:
    records = [(symbol, identity, source) for source, text, _ in sources
               for symbol, identity in annotations_in(text, source, marker)]
    if len(records) > maximum:
        raise ValueError(f"{kind} annotation count exceeds the supported limit")
    seen_ids: set[int] = set()
    seen_symbols: set[str] = set()
    for symbol, identity, source in records:
        if not re.fullmatch(r"[1-9][0-9]{0,19}", identity) or not minimum <= int(identity) <= 2**64 - 1:
            raise ValueError(f"invalid {kind} identity in {source}")
        if int(identity) in seen_ids or symbol in seen_symbols:
            raise ValueError(f"duplicate {kind} annotation in {source}")
        seen_ids.add(int(identity))
        seen_symbols.add(symbol)
    return sorted(records, key=lambda record: (int(record[1]), record[0]))


def decision_node_annotations(sources: list[tuple[pathlib.Path, str, str]]) -> list[tuple[str, str, pathlib.Path]]:
    return native_ai_annotations(sources, "HORO_AI_NODE", "decision node", 128, 1)


def perception_sense_annotations(sources: list[tuple[pathlib.Path, str, str]]) -> list[tuple[str, str, pathlib.Path]]:
    return native_ai_annotations(sources, "HORO_AI_SENSE", "perception sense", 64, 6)


def generated_sense_storage(senses: list[tuple[str, str, pathlib.Path]]) -> str:
    if not senses:
        return ""
    declarations = "\n".join(
        f"Horo::AI::PerceptionSenseRegistration HoroGeneratedPerceptionSense_{symbol}() noexcept;"
        for symbol, _, _ in senses
    )
    records = ",\n        ".join(f"HoroGeneratedPerceptionSense_{symbol}()" for symbol, _, _ in senses)
    return f'''
{declarations}
namespace {{
const auto &PerceptionSenses() {{
    static const std::array<Horo::AI::PerceptionSenseRegistration, {len(senses)}> senses{{{{
        {records}
    }}}};
    return senses;
}}
}}
'''


def validate_identity(module_id: str, fingerprint: str) -> None:
    if (
        not MODULE_ID.fullmatch(module_id)
        or len(module_id.encode("utf-8")) > MAXIMUM_IDENTITY_BYTES
        or not FINGERPRINT.fullmatch(fingerprint)
        or len(fingerprint.encode("utf-8")) > MAXIMUM_IDENTITY_BYTES
    ):
        raise ValueError("module identity or build fingerprint is invalid")


def generate_bundle(
    output: pathlib.Path,
    revision_output: pathlib.Path,
    output_root: pathlib.Path,
    module_id: str,
    fingerprint: str,
    source_paths: list[pathlib.Path],
) -> None:
    validate_identity(module_id, fingerprint)
    generated_output = confined_output(output_root, output)
    generated_revision = confined_output(output_root, revision_output)
    if generated_output == generated_revision:
        raise ValueError("generated source and revision outputs must be distinct")
    sources = read_sources(source_paths)
    annotations = [
        (symbol, type_id, source)
        for source, text, _ in sources
        for symbol, type_id in annotations_in(text, source)
    ]
    annotations.sort(key=lambda annotation: (annotation[1], annotation[0]))
    validate_annotations(annotations)
    nodes = decision_node_annotations(sources)
    senses = perception_sense_annotations(sources)
    revision = descriptor_revision(module_id, annotations, [record[2] for record in sources])
    write_atomic(generated_output, generated_source(module_id, fingerprint, revision, annotations, nodes, senses))
    write_atomic(generated_revision, f"{revision}\n")


def gameplay_main(argv=None) -> int:
    args = parse(argv)
    generate_bundle(
        args.output,
        args.revision_output,
        args.output_root,
        args.module_id,
        args.fingerprint,
        args.sources,
    )
    return 0


# Migration

#!/usr/bin/env python3


MIGRATION_SEMVER = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")
IDENTITY = re.compile(r"^[a-z][a-z0-9]*(?:[._-][a-z0-9]+)*$")
HASH = re.compile(r"^sha256:[0-9a-f]{64}$")


def migration_fail(message: str) -> None:
    raise SystemExit(f"[project-migrations] {message}")


def version_key(text: str) -> tuple[int, int, int]:
    match = MIGRATION_SEMVER.fullmatch(text)
    if match is None:
        migration_fail(f"directory is not canonical stable SemVer: {text!r}")
    return tuple(int(match.group(index)) for index in range(1, 4))


def read_manifest(path: Path) -> dict:
    resolved = path.resolve()
    try:
        if resolved.stat().st_size > 64 * 1024:
            migration_fail(f"manifest exceeds 64 KiB: {resolved}")
        value = json.loads(resolved.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        migration_fail(f"cannot read manifest {resolved}: {error}")
    if not isinstance(value, dict):
        migration_fail(f"manifest must be an object: {resolved}")
    return value


def symbol(version: str) -> str:
    return "R" + version.replace(".", "_")


def normalized_source_hash(directory: Path, manifest_name: str) -> str:
    sources = [directory / manifest_name, directory / "ProjectMigration.h", directory / "ProjectMigration.cpp"]
    stages = sorted((directory / "stages").rglob("*Stage.cpp")) if (directory / "stages").exists() else []
    sources.extend(stages)
    digest = hashlib.sha256()
    for path in sources:
        if not path.is_file():
            migration_fail(f"required migration source is missing: {path}")
        relative = path.relative_to(directory).as_posix().encode("utf-8")
        if path.name == manifest_name:
            manifest = read_manifest(path)
            manifest.pop("definitionHash", None)
            content = json.dumps(manifest, ensure_ascii=False, sort_keys=True,
                                 separators=(",", ":")).encode("utf-8")
        else:
            content = path.read_bytes().replace(b"\r\n", b"\n")
        digest.update(len(relative).to_bytes(4, "big"))
        digest.update(relative)
        digest.update(len(content).to_bytes(8, "big"))
        digest.update(content)
    return "sha256:" + digest.hexdigest()


def _validate_storage_estimate(estimate: object, manifest_path: Path) -> dict:
    if not isinstance(estimate, dict) or set(estimate) != {
            "maximumOutputRatioPermille", "maximumAddedBytesPerDocument", "maximumFixedBytes"}:
        migration_fail(f"manifest storageEstimate is invalid: {manifest_path}")
    for key in ("maximumOutputRatioPermille", "maximumAddedBytesPerDocument", "maximumFixedBytes"):
        val = estimate.get(key)
        if not isinstance(val, int) or val < 0:
            migration_fail(f"manifest storageEstimate.{key} is invalid: {manifest_path}")
    if estimate["maximumOutputRatioPermille"] < 1000:
        migration_fail(f"manifest storage estimate cannot shrink its admission bound: {manifest_path}")
    return estimate


def _validate_entry_baselines(manifest: dict, manifest_path: Path, target: str, source_from_path: str | None) -> tuple[str, str]:
    expected = {"id", "fromBaseline", "toBaseline", "sourceContract", "targetContract", "storageEstimate"}
    if not expected.issubset(manifest):
        migration_fail(f"manifest lacks required fields: {manifest_path}")
    if not isinstance(manifest["id"], str) or IDENTITY.fullmatch(manifest["id"]) is None:
        migration_fail(f"manifest has invalid stable id: {manifest_path}")
    source = manifest["fromBaseline"]
    declared_target = manifest["toBaseline"]
    if not isinstance(source, str) or not isinstance(declared_target, str):
        migration_fail(f"manifest baseline fields must be strings: {manifest_path}")
    version_key(source)
    version_key(declared_target)
    if declared_target != target or (source_from_path is not None and source != source_from_path):
        migration_fail(f"manifest source/target does not match its directory: {manifest_path}")
    if version_key(source) >= version_key(target):
        migration_fail(f"migration edge is not forward-only: {manifest_path}")
    for key in ("sourceContract", "targetContract"):
        if not isinstance(manifest[key], str) or HASH.fullmatch(manifest[key]) is None:
            migration_fail(f"manifest {key} is not canonical sha256: {manifest_path}")
    return source, target


def _validate_entry_sources(directory: Path, manifest: dict, manifest_path: Path, manifest_name: str) -> str:
    definition_hash = normalized_source_hash(directory, manifest_name)
    header = (directory / "ProjectMigration.h").read_text(encoding="utf-8")
    if "BuildProjectMigration" not in header:
        migration_fail(f"standardized BuildProjectMigration factory is missing: {directory / 'ProjectMigration.h'}")
    declared_hash = manifest.get("definitionHash")
    if declared_hash is not None and declared_hash != definition_hash:
        migration_fail(f"frozen definitionHash differs from normalized sources: {manifest_path}")
    return definition_hash


def validate_entry(directory: Path, kind: str, source_dir: str | None = None) -> dict:
    target = directory.name if kind == "sequential" else directory.parent.name
    manifest_name = "migration.horo.json" if kind == "sequential" else "checkpoint.horo.json"
    manifest_path = directory / manifest_name
    manifest = read_manifest(manifest_path)
    source, target = _validate_entry_baselines(manifest, manifest_path, target, source_dir)
    estimate = _validate_storage_estimate(manifest["storageEstimate"], manifest_path)
    definition_hash = _validate_entry_sources(directory, manifest, manifest_path, manifest_name)
    return {
        "directory": directory,
        "kind": kind,
        "id": manifest["id"],
        "source": source,
        "target": target,
        "hash": definition_hash,
        "storageEstimate": estimate,
        "namespace": symbol(target) if kind == "sequential" else f"{symbol(target)}::From{source.replace('.', '_')}",
    }


def discover(root: Path) -> list[dict]:
    entries: list[dict] = []
    definitions = root / "definitions"
    if definitions.exists():
        for target in sorted((item for item in definitions.iterdir() if item.is_dir()), key=lambda item: version_key(item.name)):
            entries.append(validate_entry(target, "sequential"))
    checkpoints = root / "checkpoints"
    if checkpoints.exists():
        for target in sorted((item for item in checkpoints.iterdir() if item.is_dir()), key=lambda item: version_key(item.name)):
            for source in sorted((item for item in target.iterdir() if item.is_dir()), key=lambda item: version_key(item.name)):
                entries.append(validate_entry(source, "checkpoint", source.name))
    identities: set[str] = set()
    edges: set[tuple[str, str, str]] = set()
    for entry in entries:
        if entry["id"] in identities:
            migration_fail(f"duplicate migration identity: {entry['id']}")
        identities.add(entry["id"])
        edge = entry["kind"], entry["source"], entry["target"]
        if edge in edges:
            migration_fail(f"duplicate migration edge: {edge}")
        edges.add(edge)
    return sorted(entries, key=lambda entry: (version_key(entry["target"]), version_key(entry["source"]), entry["kind"], entry["id"]))


def hash_initializer(value: str) -> str:
    return "{" + ",".join(f"0x{value[index:index + 2]}" for index in range(7, len(value), 2)) + "}"


def _build_support_code(current_release_path: Path | None, entries: list[dict]) -> str:
    if current_release_path is None:
        return """
Result<ProjectMigrationSupportDescriptor> BuildBuiltInProjectMigrationSupportDescriptor()
{
    return Result<ProjectMigrationSupportDescriptor>::Failure(
        MakeError(ErrorCodeDescriptor{.domain = ErrorDomainId{"horo.application.project"},
                                      .code = ErrorCode{"project.migration.support_unavailable"},
                                      .summary = "Generated migration support descriptor is unavailable."}));
}
"""
    release = read_manifest(current_release_path)
    required_release_fields = {"horoVersion", "contractBaseline", "persistentContract",
                               "minimumMigratableVersion", "migrationCheckpoints"}
    if not required_release_fields.issubset(release):
        migration_fail(f"current release manifest lacks migration support fields: {current_release_path}")
    for key in ("horoVersion", "contractBaseline", "minimumMigratableVersion"):
        if not isinstance(release[key], str):
            migration_fail(f"current release manifest {key} must be a string")
        version_key(release[key])
    if not isinstance(release["persistentContract"], str) or HASH.fullmatch(release["persistentContract"]) is None:
        migration_fail("current release manifest persistentContract is invalid")
    if not isinstance(release["migrationCheckpoints"], list) or not all(
            isinstance(value, str) for value in release["migrationCheckpoints"]):
        migration_fail("current release manifest migrationCheckpoints is invalid")
    target_entries = [entry for entry in entries if entry["kind"] == "sequential" and
                      entry["target"] == release["contractBaseline"]]
    if len(target_entries) != 1:
        migration_fail("current contract baseline requires exactly one sequential target definition")
    target = target_entries[0]
    checkpoint_lines: list[str] = []
    for checkpoint_id in release["migrationCheckpoints"]:
        matches = [entry for entry in entries if entry["kind"] == "checkpoint" and entry["id"] == checkpoint_id]
        if len(matches) != 1:
            migration_fail(f"declared checkpoint is missing or ambiguous: {checkpoint_id}")
        checkpoint = matches[0]
        checkpoint_lines.append(
            f'    support.checkpoints.push_back({{{{"{checkpoint["id"]}"}}, '
            f'{{ParseHoroVersion("{checkpoint["source"]}").Value()}}, '
            f'{{ParseHoroVersion("{checkpoint["target"]}").Value()}}}});')
    checkpoints = "\n".join(checkpoint_lines)
    return f"""
Result<ProjectMigrationSupportDescriptor> BuildBuiltInProjectMigrationSupportDescriptor()
{{
    auto target = ParseHoroVersion("{release['contractBaseline']}");
    auto minimum = ParseHoroVersion("{release['minimumMigratableVersion']}");
    auto contract = ParsePersistentContractHash("{release['persistentContract']}");
    if (target.HasError() || minimum.HasError() || contract.HasError())
        return Result<ProjectMigrationSupportDescriptor>::Failure(
            target.HasError() ? target.ErrorValue() : minimum.HasError() ? minimum.ErrorValue() : contract.ErrorValue());
    ProjectMigrationSupportDescriptor support{{
        .target = ContractBaselineVersion{{target.Value()}},
        .minimumMigratable = ContractBaselineVersion{{minimum.Value()}},
        .targetContract = contract.Value(),
        .targetValidator = Horo::ProjectMigrations::{target['namespace']}::BuildTargetValidator()
    }};
{checkpoints}
    return Result<ProjectMigrationSupportDescriptor>::Success(std::move(support));
}}
"""


def _build_sources_cmake(entries: list[dict]) -> str:
    sources: list[str] = []
    for entry in entries:
        sources.append((entry["directory"] / "ProjectMigration.cpp").as_posix())
        stages = entry["directory"] / "stages"
        if stages.exists():
            sources.extend(path.as_posix() for path in sorted(stages.rglob("*Stage.cpp")))
    return "set(HORO_PROJECT_MIGRATION_SOURCES\n" + "".join(f'    "{source}"\n' for source in sources) + ")\n"


def migration_main(argv=None) -> None:
    if argv is not None and list(argv) in (["--help"], ["-h"]):
        parser = argparse.ArgumentParser(description="Deterministic project code generation")
        parser.add_argument('migrations-root')
        parser.add_argument('output.cpp')
        parser.add_argument('sources.cmake')
        parser.add_argument('hash.txt')
        parser.add_argument('catalog.json')
        parser.add_argument('current-release.json', nargs='?')
        parser.print_help()
        return
    argsv = [sys.argv[0], *(sys.argv[1:] if argv is None else argv)]
    if len(argsv) not in (6, 7):
        migration_fail("usage: scripts/project.py migrations <root> <output.cpp> <sources.cmake> <hash.txt> <catalog.json> [current-release.json]")
    root, output_cpp, output_cmake, output_hash, output_catalog = map(Path, argsv[1:6])
    current_release_path = Path(argsv[6]) if len(argsv) == 7 else None
    entries = discover(root)
    canonical = json.dumps(
        [{key: entry[key] for key in ("kind", "id", "source", "target", "hash", "storageEstimate")} for entry in entries],
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    set_hash = "sha256:" + hashlib.sha256(canonical).hexdigest()

    includes = "\n".join(
        f'#include "src/application/project_migrations/{entry["directory"].relative_to(root).as_posix()}/ProjectMigration.h"'
        for entry in entries
    )
    calls = "\n".join(
        f"    {{ auto built = Horo::ProjectMigrations::{entry['namespace']}::BuildProjectMigration(); "
        "if (built.HasError()) return Horo::Result<std::vector<Horo::Application::ProjectMigrationDefinition>>::Failure(built.ErrorValue()); "
        "auto definition = std::move(built).Value(); "
        f"definition.hash.bytes = {hash_initializer(entry['hash'])}; "
        f"definition.storageEstimate = {{{entry['storageEstimate']['maximumOutputRatioPermille']}U, "
        f"{entry['storageEstimate']['maximumAddedBytesPerDocument']}ULL, {entry['storageEstimate']['maximumFixedBytes']}ULL}}; "
        "definitions.push_back(std::move(definition)); }"
        for entry in entries
    )
    support = _build_support_code(current_release_path, entries)

    generated = f"""// AUTO-GENERATED by scripts/project.py migrations. DO NOT EDIT.
#include \"Horo/Application/ProjectMigrationCatalog.h\"
{includes}

namespace Horo::Application
{{
Result<std::vector<ProjectMigrationDefinition>> BuildBuiltInProjectMigrationCatalog()
{{
    std::vector<ProjectMigrationDefinition> definitions;
    definitions.reserve({len(entries)});
{calls}
    return Result<std::vector<ProjectMigrationDefinition>>::Success(std::move(definitions));
}}
{support}
}} // namespace Horo::Application
"""
    cmake = _build_sources_cmake(entries)
    catalog_json = json.dumps(
        [{key: entry[key] for key in ("kind", "id", "source", "target", "hash", "storageEstimate")} for entry in entries],
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ) + "\n"
    for path, content in ((output_cpp, generated), (output_cmake, cmake),
                          (output_hash, set_hash + "\n"), (output_catalog, catalog_json)):
        resolved_path = path.resolve()
        resolved_path.parent.mkdir(parents=True, exist_ok=True)
        if not resolved_path.exists() or resolved_path.read_text(encoding="utf-8") != content:
            resolved_path.write_text(content, encoding="utf-8")


# Compatibility

#!/usr/bin/env python3


def compatibility_fail(message: str) -> None:
    raise SystemExit(f"[project-compatibility] {message}")


def parse_version(text: str) -> tuple[int, int, int, tuple[str, ...]]:
    # Bound before scanning, splitting or rendering diagnostics. SemVer is ASCII,
    # so this character limit is also the existing 64-byte wire limit.
    if len(text) > 64 or not text.isascii():
        compatibility_fail("release directory is not canonical SemVer (maximum 64 ASCII bytes)")
    core, separator, suffix = text.partition("-")
    parts = core.split(".")
    if len(parts) != 3 or any(not part or not part.isdigit() or
                             (len(part) > 1 and part[0] == "0") for part in parts):
        compatibility_fail(f"release directory is not canonical SemVer: {text!r}")
    prerelease = tuple(suffix.split(".")) if separator else ()
    for identifier in prerelease:
        if not identifier or any(not (character.isalnum() or character == "-") for character in identifier):
            compatibility_fail(f"release directory is not canonical SemVer: {text!r}")
        if identifier.isdigit() and len(identifier) > 1 and identifier[0] == "0":
            compatibility_fail(f"release has a leading-zero prerelease identifier: {text!r}")
    return int(parts[0]), int(parts[1]), int(parts[2]), prerelease


def _compare_prerelease_id(left_id: str, right_id: str) -> int:
    if left_id == right_id:
        return 0
    left_numeric, right_numeric = left_id.isdigit(), right_id.isdigit()
    if left_numeric != right_numeric:
        return -1 if left_numeric else 1
    if left_numeric:
        return -1 if int(left_id) < int(right_id) else 1
    return -1 if left_id < right_id else 1


def compare_version(left: tuple[int, int, int, tuple[str, ...]],
                    right: tuple[int, int, int, tuple[str, ...]]) -> int:
    if left[:3] != right[:3]:
        return -1 if left[:3] < right[:3] else 1
    left_pre, right_pre = left[3], right[3]
    if not left_pre or not right_pre:
        return (not left_pre) - (not right_pre)
    for left_id, right_id in zip(left_pre, right_pre):
        diff = _compare_prerelease_id(left_id, right_id)
        if diff != 0:
            return diff
    return (len(left_pre) > len(right_pre)) - (len(left_pre) < len(right_pre))


def read_json(path: Path, label: str) -> dict:
    resolved_path = path.resolve()
    try:
        document = json.loads(resolved_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        compatibility_fail(f"cannot read {label} {resolved_path}: {error}")
    if not isinstance(document, dict):
        compatibility_fail(f"{label} must be a JSON object: {resolved_path}")
    return document


def canonical_hash(document: dict, omit_release_marker: bool = False) -> str:
    payload = dict(document)
    if omit_release_marker:
        payload.pop("horoVersion", None)
    canonical = json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return f"sha256:{hashlib.sha256(canonical).hexdigest()}"


def _collect_records(releases_root: Path, engine_version: str, current_parsed: tuple, definitions_hash: str) -> list[dict]:
    records: list[dict] = []
    for directory in releases_root.iterdir():
        if not directory.is_dir():
            continue
        parsed = parse_version(directory.name)
        if compare_version(parsed, current_parsed) > 0:
            continue
        manifest = read_json(directory / "release.json", "release manifest")
        contract = read_json(directory / "project-contract.json", "project contract")
        recovery_contract = read_json(directory / "migration-recovery-contract.json", "migration recovery contract")
        if manifest.get("horoVersion") != directory.name or contract.get("horoVersion") != directory.name:
            compatibility_fail(f"release directory, manifest, and contract versions differ in {directory}")
        baseline = manifest.get("contractBaseline")
        if not isinstance(baseline, str):
            compatibility_fail(f"release manifest lacks canonical contractBaseline: {directory / 'release.json'}")
        records.append({
            "release": directory.name,
            "parsed": parsed,
            "baseline": baseline,
            "baselineParsed": parse_version(baseline),
            "contract": canonical_hash(contract, omit_release_marker=True),
            "recoveryContract": canonical_hash(recovery_contract),
            "manifest": manifest,
            "definitions": manifest.get(
                "migrationDefinitions", definitions_hash if directory.name == engine_version
                else f"sha256:{hashlib.sha256(b'[]').hexdigest()}"),
        })
    return records


def _validate_frozen_manifest(record: dict, engine_version: str, definitions_hash: str) -> None:
    frozen_contract = record["manifest"].get("persistentContract")
    frozen_recovery_contract = record["manifest"].get("migrationRecoveryContract")
    supported_recovery_contracts = record["manifest"].get("supportedMigrationRecoveryContracts")
    frozen_definitions = record["manifest"].get("migrationDefinitions")
    frozen_decision = record["manifest"].get("decisionHash")
    if frozen_contract is not None and frozen_contract != record["contract"]:
        compatibility_fail(f"release {record['release']} persistentContract differs from frozen descriptor hash")
    if frozen_recovery_contract != record["recoveryContract"]:
        compatibility_fail(f"release {record['release']} migrationRecoveryContract differs from frozen descriptor hash")
    if (not isinstance(supported_recovery_contracts, list) or
            not all(isinstance(value, dict) and
                    re.fullmatch(r"sha256:[0-9a-f]{64}", value.get("contract", "")) and
                    value.get("reader") == "journal-v1"
                    for value in supported_recovery_contracts) or
            record["recoveryContract"] not in
            [value["contract"] for value in supported_recovery_contracts]):
        compatibility_fail(f"release {record['release']} has an invalid supported recovery-contract set")
    if record["release"] == engine_version and frozen_definitions is not None and frozen_definitions != definitions_hash:
        compatibility_fail(f"release {record['release']} migrationDefinitions differs from generated definition set")
    if frozen_decision is not None and frozen_decision != record["decisionHash"]:
        compatibility_fail(f"release {record['release']} decisionHash differs from generated release decision")


def _validate_record_invariants(record: dict, baseline: dict, records: list[dict], index: int, engine_version: str, definitions_hash: str) -> None:
    if baseline["baseline"] != baseline["release"] or baseline["contract"] != record["contract"]:
        compatibility_fail(f"release {record['release']} does not match baseline contract {record['baseline']}")
    for prior in records[:index]:
        if not record["parsed"][3] and not prior["parsed"][3] and record["parsed"][:2] == prior["parsed"][:2]:
            if (prior["baseline"] != record["baseline"] or
                    prior["contract"] != record["contract"] or
                    prior["definitions"] != record["definitions"]):
                compatibility_fail(f"patch release {record['release']} changes the frozen release-line contract")
    decision = {
        "contractBaseline": record["baseline"],
        "migrationDefinitions": record["definitions"],
        "persistentContract": record["contract"],
        "release": record["release"],
    }
    record["decisionHash"] = canonical_hash(decision)
    _validate_frozen_manifest(record, engine_version, definitions_hash)


def _validate_checkpoints(current_record: dict, engine_version: str, migration_catalog: list[dict]) -> None:
    frozen_checkpoints = current_record["manifest"].get("migrationCheckpoints")
    if frozen_checkpoints is not None:
        if not isinstance(frozen_checkpoints, list) or not all(isinstance(value, str) for value in frozen_checkpoints):
            compatibility_fail(f"release {engine_version} migrationCheckpoints must be a string array")
        generated_checkpoints = sorted(
            entry["id"] for entry in migration_catalog
            if isinstance(entry, dict) and entry.get("kind") == "checkpoint" and
            entry.get("target") == engine_version and isinstance(entry.get("id"), str))
        if sorted(frozen_checkpoints) != generated_checkpoints:
            compatibility_fail(f"release {engine_version} checkpoint declarations differ from the generated catalog")


def _validate_current_version(current_record: dict, records: list[dict], engine_version: str, migration_catalog: list[dict]) -> None:
    current_index = records.index(current_record)
    minimum_migratable = current_record["manifest"].get("minimumMigratableVersion")
    if minimum_migratable is not None:
        minimum_parsed = parse_version(minimum_migratable)
        if compare_version(minimum_parsed, current_record["parsed"]) > 0:
            compatibility_fail(f"release {engine_version} minimumMigratableVersion is newer than the release")
    _validate_checkpoints(current_record, engine_version, migration_catalog)
    if current_index > 0 and current_record["baseline"] == engine_version:
        previous = records[current_index - 1]
        if previous["contract"] != current_record["contract"]:
            incoming = [entry for entry in migration_catalog
                        if isinstance(entry, dict) and entry.get("target") == engine_version]
            if not incoming:
                compatibility_fail(f"release {engine_version} changes the persistent contract without an incoming migration definition")


def _build_header_content(engine_version: str, current: dict, records: list[dict]) -> str:
    supported_recovery_entries = "\n".join(
        f'    {{"{value["contract"]}", "{value["reader"]}"}},'
        for value in current["manifest"]["supportedMigrationRecoveryContracts"]
    )
    entries = "\n".join(
        "    {\"%s\", \"%s\", \"%s\", \"%s\", %s}," % (
            record["release"], record["baseline"], record["contract"], record["decisionHash"],
            "true" if record["release"] == record["baseline"] else "false")
        for record in records
    )
    return (
        "// AUTO-GENERATED by scripts/project.py compatibility. DO NOT EDIT.\n"
        "#pragma once\n\n"
        "#include <cstddef>\n\n"
        "namespace Horo::Generated\n"
        "{\n"
        "struct ProjectCompatibilityDecision\n"
        "{\n"
        "    const char* release;\n"
        "    const char* contractBaseline;\n"
        "    const char* persistentContract;\n"
        "    const char* decisionHash;\n"
        "    bool establishesBaseline;\n"
        "};\n\n"
        f'inline constexpr char kHoroReleaseVersion[] = "{engine_version}";\n'
        f'inline constexpr char kHoroPersistentContract[] = "{current["contract"]}";\n'
        f'inline constexpr char kHoroCompatibilityDecision[] = "{current["decisionHash"]}";\n'
        f'inline constexpr char kHoroMigrationRecoveryContract[] = "{current["recoveryContract"]}";\n'
        "struct MigrationRecoveryReaderDescriptor\n"
        "{\n"
        "    const char* contract;\n"
        "    const char* reader;\n"
        "};\n"
        "inline constexpr MigrationRecoveryReaderDescriptor kHoroSupportedMigrationRecoveryContracts[] = {\n"
        f"{supported_recovery_entries}\n"
        "};\n"
        "inline constexpr std::size_t kHoroSupportedMigrationRecoveryContractCount =\n"
        "    sizeof(kHoroSupportedMigrationRecoveryContracts) /\n"
        "    sizeof(kHoroSupportedMigrationRecoveryContracts[0]);\n"
        "inline constexpr ProjectCompatibilityDecision kHoroReleaseDecisions[] = {\n"
        f"{entries}\n"
        "};\n"
        "inline constexpr std::size_t kHoroReleaseDecisionCount =\n"
        "    sizeof(kHoroReleaseDecisions) / sizeof(kHoroReleaseDecisions[0]);\n"
        "} // namespace Horo::Generated\n"
    )


def compatibility_main(argv=None) -> None:
    if argv is not None and list(argv) in (["--help"], ["-h"]):
        parser = argparse.ArgumentParser(description="Deterministic project code generation")
        parser.add_argument('releases-root')
        parser.add_argument('engine-version')
        parser.add_argument('output.h')
        parser.add_argument('migration-set-hash', nargs="?")
        parser.add_argument('migration-catalog.json', nargs="?")
        parser.print_help()
        return
    argsv = [sys.argv[0], *(sys.argv[1:] if argv is None else argv)]
    if len(argsv) not in (4, 5, 6):
        compatibility_fail("usage: scripts/project.py compatibility <releases-root> <engine-version> <output.h> [migration-set-hash] [migration-catalog.json]")

    releases_root = Path(argsv[1]).resolve()
    engine_version = argsv[2]
    output = Path(argsv[3]).resolve()
    current_parsed = parse_version(engine_version)
    definitions_hash = (argsv[4] if len(argsv) >= 5
                        else f"sha256:{hashlib.sha256(b'[]').hexdigest()}")
    if re.fullmatch(r"sha256:[0-9a-f]{64}", definitions_hash) is None:
        compatibility_fail("migration definition-set hash is not canonical SHA-256")
    migration_catalog: list[dict] = []
    if len(argsv) == 6:
        catalog_path = Path(argsv[5]).resolve()
        try:
            migration_catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            compatibility_fail(f"cannot read generated migration catalog: {error}")
        if not isinstance(migration_catalog, list):
            compatibility_fail("generated migration catalog must be an array")

    records = _collect_records(releases_root, engine_version, current_parsed, definitions_hash)
    records.sort(key=cmp_to_key(lambda left, right: compare_version(left["parsed"], right["parsed"])))
    by_release = {record["release"]: record for record in records}
    if engine_version not in by_release:
        compatibility_fail(f"no committed release decision exists for CMake PROJECT_VERSION {engine_version}")

    for index, record in enumerate(records):
        baseline = by_release.get(record["baseline"])
        if baseline is None or compare_version(record["baselineParsed"], record["parsed"]) > 0:
            compatibility_fail(f"release {record['release']} references a missing or future baseline {record['baseline']}")
        _validate_record_invariants(record, baseline, records, index, engine_version, definitions_hash)

    current_record = by_release[engine_version]
    _validate_current_version(current_record, records, engine_version, migration_catalog)

    generated = _build_header_content(engine_version, current_record, records)
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text(encoding="utf-8") != generated:
        output.write_text(generated, encoding="utf-8")  # NOSONAR



IMGUI_VULKAN_SOURCE_SHA256 = "73e622f913e664a81452a90217622298f4eed28c4e6a3cbfb1b373f23652a73f"
IMGUI_VULKAN_SIGNATURE = "void ImGui_ImplVulkan_RenderDrawData(ImDrawData* draw_data, VkCommandBuffer command_buffer, VkPipeline pipeline)"
IMGUI_VULKAN_END_MARKER = "bool ImGui_ImplVulkan_CreateFontsTexture()"
IMGUI_VULKAN_BUFFER_RING = """    if (wrb->FrameRenderBuffers == nullptr)
    {
        wrb->Index = 0;
        wrb->Count = v->ImageCount;
        wrb->FrameRenderBuffers = (ImGui_ImplVulkan_FrameRenderBuffers*)IM_ALLOC(sizeof(ImGui_ImplVulkan_FrameRenderBuffers) * wrb->Count);
        memset((void*)wrb->FrameRenderBuffers, 0, sizeof(ImGui_ImplVulkan_FrameRenderBuffers) * wrb->Count);
    }
    IM_ASSERT(wrb->Count == v->ImageCount);
    wrb->Index = (wrb->Index + 1) % wrb->Count;"""
IMGUI_VULKAN_BUFFER_GROWTH = """        if (rb->VertexBuffer == VK_NULL_HANDLE || rb->VertexBufferSize < vertex_size)
            CreateOrResizeBuffer(rb->VertexBuffer, rb->VertexBufferMemory, rb->VertexBufferSize, vertex_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (rb->IndexBuffer == VK_NULL_HANDLE || rb->IndexBufferSize < index_size)
            CreateOrResizeBuffer(rb->IndexBuffer, rb->IndexBufferMemory, rb->IndexBufferSize, index_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);"""
IMGUI_VULKAN_PREPARE = """
// Horo adaptation: native buffer storage remains frontend-owned and budgeted.
bool HoroImGuiVulkanPrepareSlots(uint32_t count)
{
    ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();
    if (bd == nullptr || count == 0 || count > 8)
        return false;
    ImGui_ImplVulkan_WindowRenderBuffers* wrb = &bd->MainWindowRenderBuffers;
    if (wrb->FrameRenderBuffers != nullptr)
        return false;
    wrb->FrameRenderBuffers = (ImGui_ImplVulkan_FrameRenderBuffers*)IM_ALLOC(sizeof(ImGui_ImplVulkan_FrameRenderBuffers) * count);
    if (wrb->FrameRenderBuffers == nullptr)
        return false;
    wrb->Count = count;
    memset((void*)wrb->FrameRenderBuffers, 0, sizeof(ImGui_ImplVulkan_FrameRenderBuffers) * count);
    return true;
}

// Binding is permitted only for a canonical retired slot, before its encoder runs.
bool HoroImGuiVulkanBindSlot(uint32_t slot, uint32_t count, VkBuffer vertices, VkDeviceMemory vertex_memory,
                            VkDeviceSize vertex_bytes, VkBuffer indices, VkDeviceMemory index_memory, VkDeviceSize index_bytes)
{
    ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();
    if (bd == nullptr || slot >= count || count != bd->MainWindowRenderBuffers.Count ||
        !vertices || !vertex_memory || !vertex_bytes || !indices || !index_memory || !index_bytes)
        return false;
    ImGui_ImplVulkan_FrameRenderBuffers* rb = &bd->MainWindowRenderBuffers.FrameRenderBuffers[slot];
    rb->VertexBuffer = vertices;
    rb->VertexBufferMemory = vertex_memory;
    rb->VertexBufferSize = vertex_bytes;
    rb->IndexBuffer = indices;
    rb->IndexBufferMemory = index_memory;
    rb->IndexBufferSize = index_bytes;
    return true;
}

// Detach borrowed native storage after drain, before upstream destroys its ring metadata.
void HoroImGuiVulkanDetachSlots()
{
    ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();
    if (bd == nullptr)
        return;
    ImGui_ImplVulkan_WindowRenderBuffers* wrb = &bd->MainWindowRenderBuffers;
    if (wrb->FrameRenderBuffers != nullptr)
        memset((void*)wrb->FrameRenderBuffers, 0, sizeof(ImGui_ImplVulkan_FrameRenderBuffers) * wrb->Count);
}

"""


def imgui_vulkan_replace_once(text: str, old: str, new: str) -> str:
    """Reject dependency drift or an ambiguous adaptation instead of silently patching it."""
    if text.count(old) != 1:
        raise ValueError("Pinned ImGui Vulkan adaptation site is absent or ambiguous")
    return text.replace(old, new, 1)


def imgui_vulkan_slots_transform(source: bytes) -> str:
    """Preserve every unrelated source byte and adapt exactly the one audited encoder."""
    if hashlib.sha256(source).hexdigest() != IMGUI_VULKAN_SOURCE_SHA256:
        raise ValueError("ImGui Vulkan source differs from reviewed revision 993fa347495860ed44b83574254ef2a317d0c14f")
    text = source.decode("utf-8")
    if text.count(IMGUI_VULKAN_SIGNATURE) != 1 or text.count(IMGUI_VULKAN_END_MARKER) != 1:
        raise ValueError("Pinned ImGui Vulkan function boundary changed")
    start, end = text.index(IMGUI_VULKAN_SIGNATURE), text.index(IMGUI_VULKAN_END_MARKER)
    function = text[start:end]
    function = imgui_vulkan_replace_once(function, IMGUI_VULKAN_SIGNATURE,
        "bool HoroImGuiVulkanRenderDrawDataForSlot(ImDrawData* draw_data, VkCommandBuffer command_buffer, uint32_t canonical_slot, uint32_t canonical_count, VkDescriptorSet (*resolve_texture)(ImTextureID, void*), void* texture_context, VkPipeline pipeline)")
    function = imgui_vulkan_replace_once(function, "        return;", "        return true;")
    function = imgui_vulkan_replace_once(function, IMGUI_VULKAN_BUFFER_RING, """    if (resolve_texture == nullptr || canonical_count == 0 || canonical_slot >= canonical_count ||
        wrb->FrameRenderBuffers == nullptr || wrb->Count != canonical_count)
        return false;
    wrb->Index = canonical_slot;""")
    function = imgui_vulkan_replace_once(function, "AlignBufferSize(draw_data->TotalVtxCount * sizeof(ImDrawVert), bd->BufferMemoryAlignment)",
        "draw_data->TotalVtxCount * sizeof(ImDrawVert)")
    function = imgui_vulkan_replace_once(function, "AlignBufferSize(draw_data->TotalIdxCount * sizeof(ImDrawIdx), bd->BufferMemoryAlignment)",
        "draw_data->TotalIdxCount * sizeof(ImDrawIdx)")
    flush_start = function.index("        VkMappedMemoryRange range[2] = {};")
    flush_end = function.index("        vkUnmapMemory", flush_start)
    function = function[:flush_start] + "        // Horo supplies host-coherent memory; no explicit flush is required.\n" + function[flush_end:]
    function = imgui_vulkan_replace_once(function, """        check_vk_result(err);
        err = vkMapMemory(v->Device, rb->IndexBufferMemory, 0, index_size, 0, (void**)&idx_dst);
        check_vk_result(err);""", """        if (err != VK_SUCCESS)
            return false;
        err = vkMapMemory(v->Device, rb->IndexBufferMemory, 0, index_size, 0, (void**)&idx_dst);
        if (err != VK_SUCCESS)
        {
            vkUnmapMemory(v->Device, rb->VertexBufferMemory);
            return false;
        }""")
    function = imgui_vulkan_replace_once(function, IMGUI_VULKAN_BUFFER_GROWTH, """        if (rb->VertexBuffer == VK_NULL_HANDLE || rb->VertexBufferSize < vertex_size ||
            rb->IndexBuffer == VK_NULL_HANDLE || rb->IndexBufferSize < index_size)
            return false;""")
    function = imgui_vulkan_replace_once(function, "                VkDescriptorSet desc_set = (VkDescriptorSet)pcmd->GetTexID();", """                VkDescriptorSet desc_set = resolve_texture(pcmd->GetTexID(), texture_context);
                if (desc_set == VK_NULL_HANDLE)
                {
                    platform_io.Renderer_RenderState = nullptr;
                    return false;
                }""")
    close = function.rfind("}")
    if close < 0 or function[close + 1:].strip():
        raise ValueError("Pinned ImGui Vulkan function tail changed")
    function = function[:close] + "    return true;\n" + function[close:]
    return text[:start] + function + IMGUI_VULKAN_PREPARE + text[end:]


def imgui_vulkan_slots_main(argv=None) -> None:
    """Write only an independent generated build source, never the fetched dependency tree."""
    parser = argparse.ArgumentParser(description="Prepare the pinned ImGui Vulkan encoder for canonical retired slots")
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    source, output = args.source.resolve(), args.output.resolve()
    if output.is_relative_to(source.parent.parent):
        parser.error("Output must be outside the fetched ImGui source tree")
    generated = imgui_vulkan_slots_transform(source.read_bytes())
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(generated, encoding="utf-8", newline="")


def verify_shader_tool_executable(root: Path, relative_path: str, digest: str) -> Path:
    """Verify the catalog-selected executable without escaping its extracted artifact root."""
    if (not relative_path or "\\" in relative_path or ":" in relative_path or
            any(part in ("", ".", "..") for part in relative_path.split("/"))):
        raise ValueError("Shader tool path must be a confined relative POSIX path")
    if re.fullmatch(r"sha256:[0-9a-f]{64}", digest) is None:
        raise ValueError("Shader tool digest must be a canonical SHA-256")
    root = root.resolve(strict=True)
    executable = (root / relative_path).resolve(strict=True)
    if not root.is_dir() or not executable.is_relative_to(root) or not executable.is_file():
        raise ValueError("Shader tool executable escapes the extracted artifact root")
    if not os.access(executable, os.X_OK) or executable.stat().st_size > MAXIMUM_FILE:
        raise ValueError("Shader tool executable is not executable or exceeds the size bound")
    actual = hashlib.sha256()
    with executable.open("rb") as source:
        while block := source.read(1024 * 1024):
            actual.update(block)
    if actual.hexdigest() != digest.removeprefix("sha256:"):
        raise ValueError("Shader tool executable does not match its reviewed digest")
    return executable


def verify_shader_tool_main(argv=None) -> None:
    """Print only the verified catalog-selected executable for the qualification host."""
    parser = argparse.ArgumentParser(description="Verify a locked extracted shader tool")
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--relative-path", required=True)
    parser.add_argument("--sha256", required=True)
    args = parser.parse_args(argv)
    print(verify_shader_tool_executable(args.root, args.relative_path, args.sha256))


TOOL_COMMANDS = {'verify-shader-tool': verify_shader_tool_main, 'imgui-vulkan-slots': imgui_vulkan_slots_main, 'android': apk_main, 'android-deploy': android_deploy_main, 'extension-bootstrap': bootstrap_main, 'extension-ci': author_main, 'extension': scaffold_main, 'gameplay': gameplay_main, 'migrations': migration_main, 'compatibility': compatibility_main}


def main(argv=None) -> int:
    """Select an explicit domain command; all implementation lives in this file."""
    argv = list(sys.argv[1:] if argv is None else argv)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=tuple(TOOL_COMMANDS))
    if not argv or argv[0] not in TOOL_COMMANDS:
        parser.parse_args(argv)
    command = argv[0]
    return TOOL_COMMANDS[command](argv[1:]) or 0


if __name__ == "__main__":
    raise SystemExit(main())
