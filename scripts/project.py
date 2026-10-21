"""Portable project authoring, Android packaging and deterministic code generation."""

from __future__ import annotations

from dataclasses import dataclass
from functools import cmp_to_key
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import io
import json
import os
import pathlib
import platform as host_platform
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import urllib.request
import zipfile


# Android

ROOT = Path(__file__).resolve().parents[1]
MAXIMUM_FILE = 256 * 1024 * 1024


# D203 and D211 conflict; retain the PEP-257 no-blank class docstring style.
class AndroidError(ValueError):  # noqa: D203
    """Actionable Android preflight or assembly failure."""


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
    if profile["permissions"] != [profile["applicationId"] + ".DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION",
                                   "android.permission.ACTIVITY_RECOGNITION"] or profile["features"]:
        raise AndroidError("Qualification profiles admit only AndroidX receiver and optional Activity Recognition permissions; no required hardware features.")
    if not re.fullmatch(r"[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+", profile["applicationId"]):
        raise AndroidError("Invalid Android application identity.")
    if not re.fullmatch(r"[a-z][a-z0-9-]*", profile["nativeLibrary"]):
        raise AndroidError("Invalid native library identity.")


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
        raise AndroidError(f"Android command failed ({return_code}); inspect {log.name}.")


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


def copy_assets(profile: dict, destination: Path) -> list[dict]:
    records = []
    for target, name in sorted(profile["assets"].items()):
        source = input_path(ROOT, name)
        output = destination / target
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(bounded_bytes(source))
        records.append({"name": target, "sha256": digest(output), "size": output.stat().st_size})
    return records


def stage_native(native_root: Path, stage: Path, tools: dict, profile: dict, abis: list[str]) -> dict:
    result = {}
    for abi in abis:
        source = native_root / abi
        records = native_closure(source, abi, tools, profile["nativeLibrary"])
        if {record["name"] for record in records} != set(profile["nativeLibraries"]):
            raise AndroidError("Native inputs differ from the explicitly declared library set.")
        destination = stage / "native" / abi
        destination.mkdir(parents=True)
        for record in records:
            shutil.copyfile(source / record["name"], destination / record["name"])
            if digest(destination / record["name"]) != record["sha256"]:
                raise AndroidError("Native input changed during staging; retry with stable files.")
        result[abi] = records
    return result


def apk_manifest(profile: dict) -> str:
    application = profile["applicationId"]  # Validated restricted identifier; cannot contain XML metacharacters.
    library = profile["nativeLibrary"]
    return f'''<manifest xmlns:android="http://schemas.android.com/apk/res/android">
  <uses-feature android:name="android.hardware.touchscreen" android:required="false" />
  <uses-permission android:name="android.permission.ACTIVITY_RECOGNITION" />
  <application android:label="Horo Package Qualification" android:theme="@style/Theme.AppCompat.NoActionBar"
               android:extractNativeLibs="false" android:allowBackup="false">
    <activity android:name="{application}.PackageActivity" android:exported="true" android:configChanges="orientation|screenSize|keyboardHidden">
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
    source = (f"package {profile['applicationId']};\n"
              "public final class PackageActivity extends org.horocore.platform.HoroInputActivity {\n"
              f"    static {{ System.loadLibrary(\"{profile['nativeLibrary']}\"); }}\n}}\n")
    (java_root / "PackageActivity.java").write_text(source, encoding="utf-8")
    input_root = stage / "java/org/horocore/platform"
    input_root.mkdir(parents=True, exist_ok=True)
    input_source = ROOT / "apps/android-input/java/org/horocore/platform/HoroInputActivity.java"
    shutil.copyfile(input_source, input_root / "HoroInputActivity.java")
    shutil.copyfile(activity, stage / "game-activity.aar")
    if digest(stage / "game-activity.aar") != tools["gameActivity"]["sha256"]:
        raise AndroidError("GameActivity changed during staging; retry with the admitted AAR.")


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
        triple = "aarch64-linux-android" if abi == "arm64-v8a" else "x86_64-linux-android"
        runtime = arguments.ndk / f"toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/{triple}/libc++_shared.so"
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
            info.compress_type = zipfile.ZIP_STORED if name.endswith(".so") else zipfile.ZIP_DEFLATED
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


def verify_assets(archive: zipfile.ZipFile, expected: dict) -> None:
    admitted = {ASSET_PREFIX + record["name"]: record for record in expected["assets"]}
    actual = {name for name in archive.namelist() if name.startswith(ASSET_PREFIX)}
    if actual != set(admitted) | {PROVENANCE_ASSET}:
        raise AndroidError("APK runtime assets differ from the declared profile.")
    for name, record in admitted.items():
        if hashlib.sha256(archive.read(name)).hexdigest() != record["sha256"]:
            raise AndroidError("APK runtime asset changed after admission.")


def assemble(arguments: argparse.Namespace) -> None:
    tools, profile = load_contract(arguments.profile, arguments.abi)
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
    native = stage_native(native_root, stage, tools, profile, arguments.abi)
    assets = copy_assets(profile, stage / "assets")
    apk_write_project(stage, tools, profile, arguments.abi, arguments.game_activity)
    os.environ["ANDROID_HOME"] = str(arguments.sdk.resolve())
    os.environ["SOURCE_DATE_EPOCH"] = "315532800"
    android_run([str(arguments.gradle.resolve()), "--no-daemon", "--console=plain", "writeDependencyProvenance"],
        stage, workspace / "java-dependencies.log")
    dependencies = stage / "assets/horo-java-dependencies.json"
    assets.append({"name": dependencies.name, "sha256": digest(dependencies), "size": dependencies.stat().st_size})
    evidence = provenance(tools, profile, native, assets)
    (stage / PROVENANCE_ASSET).write_bytes(canonical_json(evidence))
    task = "assemble" + profile["configuration"]
    android_run([str(arguments.gradle.resolve()), "--no-daemon", "--console=plain", task], stage, workspace / "gradle.log")
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
    print(json.dumps({"package": package.name, "sha256": inspection["apkSha256"], "signed": not unsigned}))


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
    parser.add_argument("--profile", choices=("qualification-debug", "qualification-release"), required=True)
    parser.add_argument("--abi", action="append", required=True)
    for name in ("sdk", "ndk", "gradle", "game-activity", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--native-root", type=Path, help="Explicit prebuilt ABI directories; complete closure is inspected before assembly.")
    parser.add_argument("--unsigned", action="store_true", help="Produce an unsigned qualification candidate; no install/signing claim.")
    arguments = parser.parse_args(argv)
    try:
        assemble(arguments)
    except (OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile) as error:
        print(f"Android preflight: {error}", file=sys.stderr)
        return 1
    return 0


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
    if (not member.filename or "\\" in member.filename or path.is_absolute() or
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


def annotations_in(text: str, source: pathlib.Path) -> list[tuple[str, str]]:
    tokens = significant_tokens(text)
    annotations: list[tuple[str, str]] = []
    expected = ("identifier", "punctuation", "identifier", "punctuation", "string", "punctuation")
    for index, token in enumerate(tokens):
        if token != ("identifier", "HORO_BEHAVIOR"):
            continue
        candidate = tokens[index : index + len(expected)]
        if len(candidate) != len(expected) or tuple(kind for kind, _ in candidate) != expected:
            raise ValueError(f"malformed HORO_BEHAVIOR annotation in {source}")
        _, opening, symbol, comma, encoded_type_id, closing = candidate
        if opening[1] != "(" or comma[1] != "," or closing[1] != ")":
            raise ValueError(f"malformed HORO_BEHAVIOR annotation in {source}")
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


def generated_exports(module_id: str, fingerprint: str, revision: int, count: int) -> str:
    behavior_pointer = "Descriptors().data()" if count else "nullptr"
    factory_pointer = "Factories().data()" if count else "nullptr"
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
    }};
    return &bundle;
}}
'''


def generated_source(
    module_id: str,
    fingerprint: str,
    revision: int,
    annotations: list[tuple[str, str, pathlib.Path]],
) -> str:
    return generated_storage(annotations) + generated_exports(module_id, fingerprint, revision, len(annotations))


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
    revision = descriptor_revision(module_id, annotations, [record[2] for record in sources])
    write_atomic(generated_output, generated_source(module_id, fingerprint, revision, annotations))
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


TOOL_COMMANDS = {'android': apk_main, 'extension-bootstrap': bootstrap_main, 'extension-ci': author_main, 'extension': scaffold_main, 'gameplay': gameplay_main, 'migrations': migration_main, 'compatibility': compatibility_main}


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
