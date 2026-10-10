"""Android assembly admission regressions; no SDK, build, network or target execution."""
import argparse
import copy
import hashlib
import json
import os
import shutil
import secrets
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import android_package
import android_contract
import android_preflight
from android_contract import AndroidError, bounded_bytes, canonical_json, input_path, load_contract, relative_name
from android_elf import inspect_elf, native_closure
from android_package import canonical_apk, inspect_package, stage_native, verify_manifest, include_generated_assets
from android_preflight import require_revision, extract_game_activity


def elf(machine=183, api=29, alignment=16384, needed=b"libc.so", rpath=False):
    """Small real ELF64 binary with load/dynamic/Android-note metadata."""
    data = bytearray(1024)
    data[:16] = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into("<HHIQQQIHHHHHH", data, 16, 3, machine, 1, 0, 64, 0, 0, 64, 56, 3, 0, 0, 0)
    struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, 0, 0, 0, 1024, 1024, alignment)
    entries = [(5, 512), (10, len(needed) + 2), (1, 1)]
    if rpath:
        entries.append((29, 1))
    entries.append((0, 0))
    struct.pack_into("<IIQQQQQQ", data, 120, 2, 4, 256, 256, 0, len(entries)*16, len(entries)*16, 8)
    struct.pack_into("<IIQQQQQQ", data, 176, 4, 4, 768, 768, 0, 24, 24, 4)
    for index, entry in enumerate(entries):
        struct.pack_into("<qQ", data, 256 + index*16, *entry)
    data[512:514+len(needed)] = b"\0" + needed + b"\0"
    struct.pack_into("<III", data, 768, 8, 4, 1)
    data[780:788] = b"Android\0"
    struct.pack_into("<I", data, 788, api)
    return bytes(data)


class AndroidPackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="horo android ü ")
        self.root = Path(self.temporary.name)
        self.tools, self.profile = load_contract("qualification-debug", ["arm64-v8a"])

    def tearDown(self):
        self.temporary.cleanup()

    def library(self, name="libentry.so", **options):
        path = self.root / name
        path.write_bytes(elf(**options))
        return path

    def test_external_version_tool_is_an_explicit_argv_capability(self):
        command = ["/host tools/gradle;echo injected", "--version"]
        result = argparse.Namespace(stdout="Gradle 8.11.1", stderr="", returncode=0)
        with patch.object(android_contract.subprocess, "run", return_value=result) as execute:
            self.assertEqual(android_contract.version_output(command), "Gradle 8.11.1")
        execute.assert_called_once_with(command, check=False, capture_output=True, text=True,
                                        timeout=30, shell=False)

    def test_tool_downloads_enforce_https_and_binary_wheels(self):
        workflow = (android_package.ROOT / ".github/workflows/android-package.yml").read_text()
        self.assertIn("--only-binary :all:", workflow)
        downloads = [line for line in workflow.splitlines() if "curl --" in line]
        self.assertEqual(len(downloads), 3)
        for line in downloads:
            self.assertIn("--proto =https --proto-redir =https", line)

    def test_runtime_dependency_lock_is_staged_and_enforced(self):
        template = android_package.ROOT / "android/gradle"
        build = (template / "build.gradle").read_text()
        self.assertIn("resolutionStrategy.activateDependencyLocking()", build)
        self.assertIn("lockMode = LockMode.STRICT", build)
        lock = (template / "gradle.lockfile").read_text()
        for module in ("androidx.appcompat:appcompat:1.7.0", "org.jetbrains.kotlin:kotlin-bom:1.8.22",
                       "org.jetbrains.kotlin:kotlin-stdlib:1.8.22"):
            self.assertIn(module + "=debugRuntimeClasspath,releaseRuntimeClasspath", lock)

    def test_real_elf_admits_declared_abi_api_and_alignment(self):
        record = inspect_elf(self.library(), 183)
        self.assertEqual(record["needed"], ["libc.so"])
        self.assertEqual(record["minimumApi"], 29)

    def test_elf_rejects_abi_api_alignment_and_search_paths(self):
        for options in ({"machine":62}, {"api":30}, {"alignment":4096}, {"rpath":True}):
            with self.subTest(options=options):
                path = self.library(**options)
                with self.assertRaises(AndroidError):
                    inspect_elf(path, 183)

    def test_truncated_elf_is_actionable(self):
        path = self.library()
        path.write_bytes(path.read_bytes()[:100])
        with self.assertRaises(AndroidError):
            inspect_elf(path, 183)

    def test_missing_native_dependency_rejected(self):
        self.library("libentry.so", needed=b"libmissing.so")
        self.library("libc++_shared.so")
        with self.assertRaisesRegex(AndroidError, "Missing native dependencies"):
            native_closure(self.root, "arm64-v8a", self.tools, "entry")

    def test_extra_native_library_rejected_before_gradle(self):
        native = self.root / "inputs" / "arm64-v8a"
        native.mkdir(parents=True)
        for name in self.profile["nativeLibraries"] + ["libundeclared.so"]:
            (native / name).write_bytes(elf())
        with self.assertRaisesRegex(AndroidError, "declared library set"):
            stage_native(native.parent, self.root / "stage", self.tools, self.profile, ["arm64-v8a"])

    def test_portable_paths_reject_traversal_absolute_and_links(self):
        self.assertEqual(relative_name("assets/ü file.json"), "assets/ü file.json")
        for name in ("../bad", "/bad", "a/../b", "a//b", "C:/bad", "a\\bad"):
            with self.subTest(name=name), self.assertRaises(AndroidError):
                relative_name(name)
        source = self.root / "ordinary"
        source.write_bytes(b"ok")
        (self.root / "link").symlink_to(source)
        with self.assertRaises(AndroidError):
            input_path(self.root, "link")

    def test_oversized_input_rejected(self):
        path = self.library()
        with self.assertRaises(AndroidError):
            bounded_bytes(path, 100)

    def test_tool_revision_drift_and_missing_are_actionable(self):
        with self.assertRaisesRegex(AndroidError, "unavailable"):
            require_revision(self.root, "28.2.13676358", "NDK")
        (self.root / "source.properties").write_text("Pkg.Revision = 27.3.13750724\n")
        with self.assertRaisesRegex(AndroidError, "version drift"):
            require_revision(self.root, "28.2.13676358", "NDK")

    def test_game_activity_digest_rejected_before_extraction(self):
        path = self.root / "dependency.aar"
        path.write_bytes(b"unexpected dependency")
        with self.assertRaisesRegex(AndroidError, "digest mismatch"):
            extract_game_activity(path, self.root / "extracted", self.tools, ["arm64-v8a"])
        self.assertFalse((self.root / "extracted").exists())

    def evidence(self):
        payload = b"native input"
        record = {"name":"libentry.so", "sha256":hashlib.sha256(payload).hexdigest()}
        return {"abis":{"arm64-v8a":[record]}, "assets":[]}, payload

    def package(self, path, evidence, native):
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("AndroidManifest.xml", b"manifest")
            archive.writestr("classes.dex", b"dex")
            archive.writestr("assets/horo-package-provenance.json", canonical_json(evidence))
            archive.writestr("lib/arm64-v8a/libentry.so", native)

    def test_apk_payload_identity_and_mutation(self):
        evidence, native = self.evidence()
        package = self.root / "package.apk"
        self.package(package, evidence, native)
        self.assertEqual(len(inspect_package(package, evidence)["contents"]), 4)
        self.package(package, evidence, b"changed")
        with self.assertRaisesRegex(AndroidError, "changed after"):
            inspect_package(package, evidence)

    def test_canonical_archive_repeatability_and_stored_native(self):
        evidence, native = self.evidence()
        source = self.root / "source.apk"
        self.package(source, evidence, native)
        first, second = self.root / "first.apk", self.root / "second.apk"
        canonical_apk(source, first)
        canonical_apk(source, second)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        with zipfile.ZipFile(first) as archive:
            self.assertEqual(archive.getinfo("lib/arm64-v8a/libentry.so").compress_type, zipfile.ZIP_STORED)

    def test_declared_release_generated_assets_are_bound_to_provenance(self):
        evidence, native = self.evidence()
        evidence["profile"] = {"generatedAssets":["dexopt/baseline.prof","dexopt/baseline.profm"]}
        source = self.root / "source.apk"
        self.package(source,evidence,native)
        with zipfile.ZipFile(source,"a") as archive:
            for name in evidence["profile"]["generatedAssets"]:
                archive.writestr("assets/"+name,b"generated baseline profile")
        include_generated_assets(source,evidence)
        self.assertEqual(len(evidence["assets"]),2)
        self.assertEqual(evidence["assets"][0]["origin"],"androidGradlePlugin")
        canonical = self.root / "bound.apk"
        canonical_apk(source,canonical,canonical_json(evidence))
        self.assertEqual(len(inspect_package(canonical,evidence)["contents"]),6)
        with zipfile.ZipFile(canonical,"a") as archive:
            archive.writestr("assets/undeclared.json",b"extra")
        with self.assertRaisesRegex(AndroidError,"runtime assets differ"):
            inspect_package(canonical,evidence)

    def test_missing_declared_generated_assets_rejected(self):
        evidence, native = self.evidence()
        evidence["profile"] = {"generatedAssets":["dexopt/baseline.prof","dexopt/baseline.profm"]}
        source = self.root / "source.apk"
        self.package(source,evidence,native)
        with self.assertRaisesRegex(AndroidError,"undeclared generated"):
            include_generated_assets(source,evidence)

    def test_manifest_rejects_permission_feature_and_api_drift(self):
        text = ("package: name='org.horocore.packagequalification' versionCode='1' versionName='0.2.0'\n"
                "minSdkVersion:'29'\ntargetSdkVersion:'36'\n"
                "uses-permission: name='org.horocore.packagequalification.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION'\n")
        verify_manifest(text, self.profile)
        for changed in (text.replace("'29'", "'30'"), text + "uses-permission: name='android.permission.CAMERA'\n",
                        text + "uses-feature: name='android.hardware.vulkan.level'\n"):
            with self.assertRaises(AndroidError):
                verify_manifest(changed, self.profile)

    def test_current_aapt2_min_sdk_badging_and_ambiguous_alias(self):
        text = ("package: name='org.horocore.packagequalification' versionCode='1' versionName='0.2.0'\n"
                "minSdkVersion:'29'\ntargetSdkVersion:'36'\n"
                "uses-permission: name='org.horocore.packagequalification.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION'\n")
        verify_manifest(text, self.profile)
        ambiguous = text + "sdkVersion:'29'\n"
        with self.assertRaisesRegex(AndroidError, "profile API 29"):
            verify_manifest(ambiguous, self.profile)
        wrong_api = text.replace("'29'", "'30'")
        with self.assertRaisesRegex(AndroidError, "profile API 29"):
            verify_manifest(wrong_api, self.profile)

    def test_profiles_fail_closed_on_undeclared_abi(self):
        for profile, abis in (("qualification-debug", ["armeabi-v7a"]), ("qualification-release", ["x86_64"]),
                              ("qualification-debug", ["arm64-v8a", "arm64-v8a"])):
            with self.assertRaises(AndroidError):
                load_contract(profile, abis)


class AndroidAssemblyTests(unittest.TestCase):
    setUp = AndroidPackageTests.setUp
    tearDown = AndroidPackageTests.tearDown

    def arguments(self):
        return argparse.Namespace(profile="qualification-debug", abi=["arm64-v8a"],
                                  sdk=self.root / "sdk", ndk=self.root / "ndk",
                                  gradle=self.root / "gradle", game_activity=self.root / "activity.aar",
                                  output=self.root / "output", native_root=self.root / "prebuilt", unsigned=True)

    def badging(self):
        return ("package: name='org.horocore.packagequalification' versionCode='1' versionName='0.2.0'\n"
                "minSdkVersion:'29'\ntargetSdkVersion:'36'\n"
                "uses-permission: name='org.horocore.packagequalification.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION'\n")

    def prepare(self, arguments):
        native = arguments.native_root / "arm64-v8a"
        native.mkdir(parents=True)
        for name in self.profile["nativeLibraries"]:
            (native / name).write_bytes(elf())
        metadata = {"abi":"arm64-v8a", "api":21, "stl":"c++_shared", "static":True, "ndk":23}
        with zipfile.ZipFile(arguments.game_activity, "w") as archive:
            archive.writestr("prefab/modules/game-activity_static/libs/android.arm64-v8a/abi.json", json.dumps(metadata))
        tools = copy.deepcopy(self.tools)
        tools["gameActivity"]["sha256"] = hashlib.sha256(arguments.game_activity.read_bytes()).hexdigest()
        return tools

    def command(self, argv, directory, log):
        log.write_text("successful qualification command\n")
        if argv[-1] == "writeDependencyProvenance":
            (directory / "assets/horo-java-dependencies.json").write_bytes(b"[]\n")
        elif argv[-1] == "assembleDebug":
            destination = directory / "build/outputs/apk/debug/application.apk"
            destination.parent.mkdir(parents=True)
            with zipfile.ZipFile(destination, "w") as archive:
                archive.writestr("AndroidManifest.xml", b"binary manifest")
                archive.writestr("classes.dex", b"dex")
                for root_name, prefix in (("native", "lib"), ("assets", "assets")):
                    for source in sorted((directory / root_name).rglob("*")):
                        if source.is_file():
                            archive.writestr(prefix + "/" + source.relative_to(directory / root_name).as_posix(), source.read_bytes())
        elif "-f" in argv:
            shutil.copyfile(argv[-2], argv[-1])
        elif "sign" in argv:
            shutil.copyfile(argv[-1], argv[argv.index("--out") + 1])

    def test_assembly_real_staging_and_content_inspection(self):
        arguments = self.arguments()
        tools = self.prepare(arguments)
        capability = {"commands":{name:name for name in ("zipalign", "apksigner", "aapt2")},
                      "signing":{"keystore":"external.p12", "alias":"qualification"}}
        with patch.object(android_package, "load_contract", return_value=(tools,self.profile)), \
             patch.object(android_package, "tools_preflight", return_value=capability), \
             patch.object(android_package, "run", side_effect=self.command), \
             patch.object(android_package, "version_output", side_effect=lambda argv: self.badging() if "badging" in argv else "abc123"):
            android_package.assemble(arguments)
            inspection = json.loads((arguments.output / "inspection.json").read_text())
            self.assertFalse(inspection["signed"])
            evidence = json.loads((arguments.output / "provenance.json").read_text())
            self.assertEqual(evidence["sourceRevision"], "abc123")
            self.assertNotIn(str(self.root), json.dumps(evidence))
            arguments.output = self.root / "signed-output"
            arguments.unsigned = False
            android_package.assemble(arguments)
            self.assertTrue(json.loads((arguments.output / "inspection.json").read_text())["signed"])
            with self.assertRaisesRegex(AndroidError, "Output already exists"):
                android_package.assemble(arguments)

    def test_kotlin_runtime_alignment_is_locked_before_assembly(self):
        template = (Path(__file__).resolve().parents[2] / "android/gradle/build.gradle").read_text()
        self.assertEqual(self.tools["kotlin"], "1.8.22")
        self.assertIn('org.jetbrains.kotlin:kotlin-bom:${contract.tools.kotlin}', template)
        self.assertIn('component.version != contract.tools.kotlin', template)
        self.assertIn('Kotlin runtime drift', template)

    def test_hosted_ninja_install_uses_lock_and_explicit_path(self):
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/android-package.yml").read_text()
        self.assertIn('["ninja"]', workflow)
        self.assertIn("lock['ninjaSha256']", workflow)
        self.assertIn('"$RUNNER_TEMP/android-bin" >> "$GITHUB_PATH"', workflow)
        self.assertNotIn('ninja==', workflow)
        self.assertEqual(len(self.tools["ninjaSha256"]), 64)

    def test_tool_preflight_actual_metadata_and_version_commands(self):
        arguments = self.arguments()
        for root, revision in ((arguments.ndk,self.tools["ndk"]),
                               (arguments.sdk / "build-tools" / self.tools["buildTools"],self.tools["buildTools"])):
            root.mkdir(parents=True)
            (root / "source.properties").write_text("Pkg.Revision = " + revision + "\n")
        platform = arguments.sdk / "platforms" / "android-36"
        platform.mkdir(parents=True)
        (platform / "android.jar").write_bytes(b"sdk")
        for name in ("zipalign", "apksigner", "aapt2"):
            tool = arguments.sdk / "build-tools" / self.tools["buildTools"] / name
            tool.write_bytes(b"tool")
            tool.chmod(0o700)
        outputs = ['cmake version 3.31.6', '1.11.1', 'Gradle 8.11.1', 'openjdk version "17.0.1"']
        with patch.object(android_preflight.sys, "platform", "linux"), \
             patch.object(android_preflight, "version_output", side_effect=outputs):
            capability = android_preflight.tools_preflight(self.tools,arguments.sdk,arguments.ndk,arguments.gradle,False)
            self.assertIsNone(capability["signing"])
        with patch.object(android_preflight.sys, "platform", "darwin"), self.assertRaisesRegex(AndroidError, "Linux"):
            android_preflight.tools_preflight(self.tools,arguments.sdk,arguments.ndk,arguments.gradle,False)
        with patch.object(android_preflight, "version_output", return_value="wrong"), self.assertRaisesRegex(AndroidError,"version drift"):
            android_preflight.require_version(["cmake"], "pinned", "CMake")

    def test_signing_capability_missing_invalid_and_valid(self):
        with patch.dict(os.environ, {}, clear=True), self.assertRaisesRegex(AndroidError,"Signing profile unavailable"):
            android_preflight.signing_inputs()
        keystore = self.root / "external.p12"
        keystore.write_bytes(b"qualification fixture")
        disposable_password = secrets.token_hex(16)
        identity = {"HORO_ANDROID_KEYSTORE":str(keystore), "HORO_ANDROID_KEY_ALIAS":"fixture",
                    "HORO_ANDROID_STORE_PASSWORD":disposable_password, "HORO_ANDROID_KEY_PASSWORD":disposable_password}
        with patch.dict(os.environ,identity), patch.object(android_preflight,"version_output",return_value="alias present"):
            self.assertEqual(android_preflight.signing_inputs()["alias"], "fixture")
        with patch.dict(os.environ,identity), patch.object(android_preflight,"version_output",side_effect=AndroidError("invalid")), \
             self.assertRaisesRegex(AndroidError,"Signing identity cannot be opened"):
            android_preflight.signing_inputs()

    def test_native_configure_commands_are_abi_specific(self):
        arguments = self.arguments()
        arguments.native_root = None
        runtime = arguments.ndk / "toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
        runtime.parent.mkdir(parents=True)
        runtime.write_bytes(elf())
        workspace = self.root / "native-build"
        workspace.mkdir()
        commands = []

        def build(argv, directory, log):
            commands.append(argv)
            if "--build" in argv:
                output = Path(argv[argv.index("--build")+1])
                output.mkdir(parents=True)
                (output / "libhoro-package-qualification.so").write_bytes(elf())
        with patch.dict(os.environ,{},clear=False), patch.object(android_package,"run",side_effect=build):
            native = android_package.build_native(arguments,self.profile,self.root / "activity",workspace)
            self.assertTrue((native / "arm64-v8a/libc++_shared.so").is_file())
            self.assertIn("android-debug",commands[0])
            self.assertEqual(os.environ["HORO_ANDROID_ABI"],"arm64-v8a")

    def test_dependency_archive_traversal_and_api_drift(self):
        arguments = self.arguments()
        tools = self.prepare(arguments)
        with zipfile.ZipFile(arguments.game_activity,"w") as archive:
            archive.writestr("../escape",b"bad")
        tools["gameActivity"]["sha256"] = hashlib.sha256(arguments.game_activity.read_bytes()).hexdigest()
        with self.assertRaises(AndroidError):
            extract_game_activity(arguments.game_activity,self.root / "extract",tools,["arm64-v8a"])
        self.assertFalse((self.root / "escape").exists())


if __name__ == "__main__":
    unittest.main()
