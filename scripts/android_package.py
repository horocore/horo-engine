#!/usr/bin/env python3
"""Reproducible, explicitly composed Android APK assembly and portable content inspection."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import zipfile

from android_contract import AndroidError, ROOT, bounded_bytes, canonical_json, digest, input_path, load_contract, relative_name, run, version_output
from android_elf import native_closure
from android_preflight import extract_game_activity, tools_preflight


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


def manifest(profile: dict) -> str:
    application = profile["applicationId"]  # Validated restricted identifier; cannot contain XML metacharacters.
    library = profile["nativeLibrary"]
    return f'''<manifest xmlns:android="http://schemas.android.com/apk/res/android">
  <uses-feature android:name="android.hardware.touchscreen" android:required="false" />
  <application android:label="Horo Package Qualification" android:theme="@style/Theme.AppCompat.NoActionBar"
               android:extractNativeLibs="false" android:allowBackup="false">
    <activity android:name="{application}.PackageActivity" android:exported="true" android:configChanges="orientation|screenSize|keyboardHidden">
      <meta-data android:name="android.app.lib_name" android:value="{library}" />
      <intent-filter><action android:name="android.intent.action.MAIN" /><category android:name="android.intent.category.LAUNCHER" /></intent-filter>
    </activity>
  </application>
</manifest>
'''


def write_project(stage: Path, tools: dict, profile: dict, abis: list[str], activity: Path) -> None:
    for source in (ROOT / "android/gradle").iterdir():
        shutil.copyfile(source, stage / source.name)
    (stage / "contract.json").write_bytes(canonical_json({"tools": tools, "profile": profile, "abis": sorted(abis)}))
    (stage / "AndroidManifest.xml").write_text(manifest(profile), encoding="utf-8")
    java_root = stage / "java" / Path(*profile["applicationId"].split("."))
    java_root.mkdir(parents=True)
    source = (f"package {profile['applicationId']};\n"
              "public final class PackageActivity extends com.google.androidgamesdk.GameActivity {\n"
              f"    static {{ System.loadLibrary(\"{profile['nativeLibrary']}\"); }}\n}}\n")
    (java_root / "PackageActivity.java").write_text(source, encoding="utf-8")
    shutil.copyfile(activity, stage / "game-activity.aar")
    if digest(stage / "game-activity.aar") != tools["gameActivity"]["sha256"]:
        raise AndroidError("GameActivity changed during staging; retry with the admitted AAR.")


def build_native(arguments: argparse.Namespace, tools: dict, profile: dict, activity_root: Path, workspace: Path) -> Path:
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
        run(["cmake", "--preset", profile["preset"], "-B", str(binary)], ROOT, workspace / f"configure-{abi}.log")
        run(["cmake", "--build", str(binary), "--target", "HoroAndroidPackageQualification", "--parallel", "2"],
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
            "abis": abis, "assets": assets, "sourceDateEpoch": 315532800}


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
            payload = provenance_bytes if name == "assets/horo-package-provenance.json" and provenance_bytes is not None else original.read(name)
            result.writestr(info, payload)


def sign_package(package: Path, signed: Path, tools: dict, capability: dict, workspace: Path) -> None:
    identity = capability["signing"]
    run([capability["commands"]["apksigner"], "sign", "--ks", identity["keystore"], "--ks-key-alias", identity["alias"],
         "--ks-pass", "env:HORO_ANDROID_STORE_PASSWORD", "--key-pass", "env:HORO_ANDROID_KEY_PASSWORD",
         "--v1-signing-enabled", "false", "--v2-signing-enabled", "true", "--v3-signing-enabled", "false",
         "--min-sdk-version", str(tools["minimumApi"]), "--out", str(signed), str(package)], workspace, workspace / "sign.log")
    run([capability["commands"]["apksigner"], "verify", "--verbose", str(signed)], workspace, workspace / "verify-signature.log")


def inspect_package(package: Path, expected: dict) -> dict:
    with zipfile.ZipFile(package) as archive:
        archive_limits(archive)
        names = archive.namelist()
        if len(names) != len(set(names)) or "classes.dex" not in names or "AndroidManifest.xml" not in names:
            raise AndroidError("APK is ambiguous or missing its Android application payload.")
        provenance_bytes = archive.read("assets/horo-package-provenance.json")
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
    admitted = {f"assets/{record['name']}": record for record in expected["assets"]}
    actual = {name for name in archive.namelist() if name.startswith("assets/")}
    if actual != set(admitted) | {"assets/horo-package-provenance.json"}:
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
    native_root = build_native(arguments, tools, profile, activity_root, workspace)
    stage = workspace / "gradle-project"
    stage.mkdir()
    native = stage_native(native_root, stage, tools, profile, arguments.abi)
    assets = copy_assets(profile, stage / "assets")
    write_project(stage, tools, profile, arguments.abi, arguments.game_activity)
    os.environ["ANDROID_HOME"] = str(arguments.sdk.resolve())
    os.environ["SOURCE_DATE_EPOCH"] = "315532800"
    run([str(arguments.gradle.resolve()), "--no-daemon", "--console=plain", "writeDependencyProvenance"],
        stage, workspace / "java-dependencies.log")
    dependencies = stage / "assets/horo-java-dependencies.json"
    assets.append({"name": dependencies.name, "sha256": digest(dependencies), "size": dependencies.stat().st_size})
    evidence = provenance(tools, profile, native, assets)
    (stage / "assets/horo-package-provenance.json").write_bytes(canonical_json(evidence))
    task = "assemble" + profile["configuration"]
    run([str(arguments.gradle.resolve()), "--no-daemon", "--console=plain", task], stage, workspace / "gradle.log")
    complete_package(stage, workspace, profile, tools, capability, evidence, arguments.unsigned)


def include_generated_assets(package: Path, evidence: dict) -> None:
    """Bind explicitly declared Gradle-produced runtime assets before canonical alignment/signing."""
    with zipfile.ZipFile(package) as archive:
        archive_limits(archive)
        if archive.read("assets/horo-package-provenance.json") != canonical_json(evidence):
            raise AndroidError("Gradle changed the admitted source provenance before final assembly.")
        generated = evidence["profile"]["generatedAssets"]
        ordinary = {"assets/" + record["name"] for record in evidence["assets"]}
        expected = ordinary | {"assets/" + name for name in generated} | {"assets/horo-package-provenance.json"}
        actual = {name for name in archive.namelist() if name.startswith("assets/")}
        if actual != expected:
            raise AndroidError("Gradle output has absent or undeclared generated runtime assets.")
        for name in generated:
            payload = archive.read("assets/" + name)
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
    run([capability["commands"]["zipalign"], "-P", "16", "-f", "4", str(canonical), str(aligned)], workspace, workspace / "align.log")
    package = aligned
    if not unsigned:
        package = workspace / "signed.apk"
        sign_package(aligned, package, tools, capability, workspace)
    run([capability["commands"]["zipalign"], "-c", "-P", "16", "4", str(package)], workspace, workspace / "verify-align.log")
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("qualification-debug", "qualification-release"), required=True)
    parser.add_argument("--abi", action="append", required=True)
    for name in ("sdk", "ndk", "gradle", "game-activity", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--native-root", type=Path, help="Explicit prebuilt ABI directories; complete closure is inspected before assembly.")
    parser.add_argument("--unsigned", action="store_true", help="Produce an unsigned qualification candidate; no install/signing claim.")
    arguments = parser.parse_args()
    try:
        assemble(arguments)
    except (AndroidError, OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile) as error:
        print(f"Android preflight: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
