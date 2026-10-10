"""Fail closed on missing/drifting tools, native SDK inputs and signing capabilities."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import sys
import zipfile

from android_contract import AndroidError, bounded_bytes, digest, relative_name, version_output


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
