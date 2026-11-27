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
from xml.etree import ElementTree

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import project as android_package
import project as android_contract
import project as android_preflight
from project import AndroidError as AndroidError, bounded_bytes as bounded_bytes, canonical_json as canonical_json, input_path as input_path, load_contract as load_contract, relative_name as relative_name
from project import inspect_elf as inspect_elf, native_closure as native_closure
from project import canonical_apk as canonical_apk, inspect_package as inspect_package, stage_native as stage_native, verify_manifest as verify_manifest, include_generated_assets as include_generated_assets
from project import require_revision as require_revision, extract_game_activity as extract_game_activity


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
        executable = self.root / "host tools/gradle;echo injected"
        executable.parent.mkdir()
        executable.write_bytes(b"disposable executable fixture; execution is mocked")
        executable.chmod(0o700)
        command = [str(executable), "--version"]
        result = argparse.Namespace(stdout="Gradle 8.11.1", stderr="", returncode=0)
        environment = {"HORO_TEST_TOOL": "disposable"}
        with patch.dict(os.environ, environment, clear=True), patch.object(
                android_contract.subprocess, "run", return_value=result) as execute:
            self.assertEqual(android_contract.version_output(command), "Gradle 8.11.1")
        expected_environment = dict(environment)
        if os.name == "nt":
            expected_environment["NoDefaultCurrentDirectoryInExePath"] = "1"
        execute.assert_called_once_with(command, check=False, capture_output=True, text=True,
                                        timeout=30, shell=False, env=expected_environment)

    def test_tool_downloads_enforce_https_and_binary_wheels(self):
        workflow = (android_package.ROOT / ".github/workflows/android-package.yml").read_text()
        self.assertIn("--only-binary :all:", workflow)
        downloads = [line for line in workflow.splitlines() if "curl --" in line]
        self.assertEqual(len(downloads), 4)
        for line in downloads:
            self.assertIn("--proto =https --proto-redir =https", line)

    def test_runtime_dependency_lock_is_staged_and_enforced(self):
        template = android_package.ROOT / "android/gradle"
        build = (template / "build.gradle").read_text()
        self.assertIn("resolutionStrategy.activateDependencyLocking()", build)
        self.assertIn("lockMode = LockMode.STRICT", build)
        lock = (template / "gradle.lockfile").read_text()
        for module in ("androidx.appcompat:appcompat:1.7.0", "org.jetbrains.kotlin:kotlin-bom:1.8.22",
                       "org.jetbrains.kotlin:kotlin-stdlib:1.8.22", "org.jetbrains.kotlinx:kotlinx-coroutines-bom:1.6.4"):
            self.assertIn(module + "=debugRuntimeClasspath,releaseRuntimeClasspath", lock)

    def test_dependency_resolution_retries_throttling_and_preserves_logs(self):
        log = self.root / "java-dependencies.log"
        responses = iter(("Received status code 429 from server: Too Many Requests", "BUILD SUCCESSFUL"))

        def gradle(command, directory, output):
            self.assertEqual(command[-1], "writeDependencyProvenance")
            self.assertEqual(directory, self.root)
            response = next(responses)
            output.write_text(response, encoding="utf-8")
            if "429" in response:
                raise android_package.AndroidCommandFailed("dependency download failed")

        with patch.object(android_package, "android_run", side_effect=gradle) as run, patch.object(
                android_package.time, "sleep") as sleep:
            android_package.run_gradle_task(self.root / "gradle", "writeDependencyProvenance", self.root, log)
        self.assertEqual(run.call_count, 2)
        sleep.assert_called_once_with(20)
        self.assertEqual(log.read_text(encoding="utf-8"), "BUILD SUCCESSFUL")
        self.assertIn("429", (self.root / "java-dependencies-attempt-1.log").read_text(encoding="utf-8"))

    def test_release_assembly_retries_lint_artifact_throttling(self):
        log = self.root / "gradle.log"
        throttled = (
            "> Task :lintVitalAnalyzeRelease FAILED\n"
            "> Could not resolve org.codehaus.groovy:groovy:3.0.22.\n"
            "> Received status code 429 from server: Too Many Requests\n"
        )
        responses = iter((throttled, throttled, "BUILD SUCCESSFUL"))

        def gradle(command, directory, output):
            self.assertEqual(command[-1], "assembleRelease")
            self.assertEqual(directory, self.root)
            response = next(responses)
            output.write_text(response, encoding="utf-8")
            if response == throttled:
                raise android_package.AndroidCommandFailed("release dependency throttled")

        with patch.object(android_package, "android_run", side_effect=gradle) as run, patch.object(
                android_package.time, "sleep") as sleep:
            android_package.run_gradle_task(self.root / "gradle", "assembleRelease", self.root, log)
        self.assertEqual(run.call_count, 3)
        self.assertEqual([invocation.args for invocation in sleep.call_args_list], [(20,), (60,)])
        self.assertEqual(log.read_text(encoding="utf-8"), "BUILD SUCCESSFUL")
        for attempt in (1, 2):
            self.assertEqual((self.root / f"gradle-attempt-{attempt}.log").read_text(encoding="utf-8"), throttled)
    def test_dependency_resolution_fails_immediately_for_lock_error(self):
        log = self.root / "java-dependencies.log"

        def gradle(_command, _directory, output):
            output.write_text("Dependency lock state does not match", encoding="utf-8")
            raise android_package.AndroidCommandFailed("dependency lock error")

        with patch.object(android_package, "android_run", side_effect=gradle) as run, patch.object(
                android_package.time, "sleep") as sleep, self.assertRaisesRegex(AndroidError, "dependency lock error"):
            android_package.run_gradle_task(self.root / "gradle", "writeDependencyProvenance", self.root, log)
        run.assert_called_once()
        sleep.assert_not_called()
        self.assertEqual(log.read_text(encoding="utf-8"), "Dependency lock state does not match")

    def test_vulkan_development_profile_preserves_declared_effective_constraints(self):
        _, profile = load_contract("qualification-vulkan-development", ["arm64-v8a", "x86_64"])
        self.assertEqual(profile["renderer"], "vulkan-android")
        self.assertFalse(profile["distributionQualified"])
        with patch.object(android_package, "version_output", return_value="fixture-source-revision"):
            evidence = android_package.provenance(self.tools, profile, {}, [])
        self.assertEqual(evidence["profile"], profile)
        self.assertTrue(evidence["profile"]["graphicsProfile"]["swapchainMaintenance1Required"])
        self.assertEqual(evidence["profile"]["graphicsProfile"]["schema"], 2)
        self.assertEqual(evidence["profile"]["graphicsProfile"]["compositeAlphaPolicy"],
                         "prefer-opaque-else-inherit-alpha-one")

    def test_vulkan_development_profile_reaches_package_assembly_from_cli(self):
        arguments = ["--profile", "qualification-vulkan-development", "--abi", "arm64-v8a"]
        for name in ("sdk", "ndk", "gradle", "game-activity", "output"):
            arguments.extend(["--" + name, str(self.root / name)])
        with patch.object(android_package, "assemble") as assemble:
            self.assertEqual(android_package.main(["android", *arguments]), 0)
        parsed = assemble.call_args.args[0]
        self.assertEqual(parsed.profile, "qualification-vulkan-development")
        self.assertEqual(parsed.abi, ["arm64-v8a"])
        self.assertEqual(parsed.output, self.root / "output")

    def test_vulkan_development_asset_declares_intent_without_device_qualification(self):
        _, profile = load_contract("qualification-vulkan-development", ["arm64-v8a"])
        source = input_path(android_package.ROOT, profile["assets"]["qualification.json"])
        intent = json.loads(bounded_bytes(source))
        self.assertIs(intent["presentation"], True)
        self.assertIs(intent["deviceQualified"], False)
        old_source = input_path(android_package.ROOT, self.profile["assets"]["qualification.json"])
        self.assertNotEqual(source, old_source)
        self.assertIs(json.loads(bounded_bytes(old_source))["presentation"], False)

    def test_vulkan_development_profile_rejects_policy_drift(self):
        _, profile = load_contract("qualification-vulkan-development", ["arm64-v8a"])
        invalid = [dict(profile, renderer="vulkan"), dict(profile, distributionQualified=True),
                   dict(profile, preset="android-debug"), dict(profile, configuration="Release")]
        graphics = dict(profile["graphicsProfile"], swapchainMaintenance1Required=False)
        invalid.append(dict(profile, graphicsProfile=graphics))
        invalid.append(dict(profile, graphicsProfile=dict(profile["graphicsProfile"], unknown=True)))
        invalid.append(dict(profile, graphicsProfile=dict(profile["graphicsProfile"], swapchainMaintenance1Required=1)))
        invalid.append(dict(profile, graphicsProfile=dict(profile["graphicsProfile"], schema=True)))
        invalid.append(dict(profile, graphicsProfile=dict(profile["graphicsProfile"], schema=2.0)))
        invalid.append(dict(profile, graphicsProfile=dict(profile["graphicsProfile"], schema=1)))
        invalid.append(dict(profile, graphicsProfile=dict(profile["graphicsProfile"], compositeAlphaPolicy="opaque-only")))
        invalid.append(dict(profile, assets={"qualification.json": "android/qualification.json"}))
        for candidate in invalid:
            with self.subTest(candidate=candidate), self.assertRaises(AndroidError):
                android_package.validate_profile(candidate, self.tools, ["arm64-v8a"])

    def test_assembly_only_profile_cannot_claim_graphics_profile(self):
        profile = dict(self.profile, graphicsProfile={"schema": 1})
        with self.assertRaises(AndroidError):
            android_package.validate_profile(profile, self.tools, ["arm64-v8a"])

    def test_openxr_development_manifest_and_loader_lock_are_profile_scoped(self):
        name = android_package.OPENXR_DEVELOPMENT_PROFILE
        tools, profile = load_contract(name, ["arm64-v8a", "x86_64"])
        self.assertFalse(profile["distributionQualified"])
        self.assertEqual(profile["renderer"], "none")
        self.assertEqual(profile["openxrProfile"]["activityRoute"], "cold-discovery")
        self.assertEqual(profile["preset"], "android-openxr-development")
        self.assertEqual(tools["openxrLoader"]["artifact"], "org.khronos.openxr:openxr_loader_for_android")
        self.assertNotIn("openxrLoader", self.tools)
        attr = "{http://schemas.android.com/apk/res/android}"
        manifest = ElementTree.fromstring(android_package.apk_manifest(profile))
        self.assertEqual({item.attrib[attr + "name"] for item in manifest.findall("uses-permission")},
                         set(profile["permissions"][1:]))
        self.assertEqual(manifest.find("queries/provider").attrib[attr + "authorities"],
                         "org.khronos.openxr.runtime_broker;org.khronos.openxr.system_runtime_broker")
        self.assertEqual({item.attrib[attr + "name"] for item in manifest.findall("queries/intent/action")},
                         {"org.khronos.openxr.OpenXRRuntimeService", "org.khronos.openxr.OpenXRApiLayerService"})
        self.assertIsNone(ElementTree.fromstring(android_package.apk_manifest(self.profile)).find("queries"))
        changed = dict(profile, openxrProfile=dict(profile["openxrProfile"], activityRoute="available"))
        with self.assertRaisesRegex(AndroidError, "cold-discovery preset"):
            android_package.validate_profile(changed, tools, ["arm64-v8a"])

    def test_quest_manifest_declares_required_passthrough_only_in_dedicated_arm64_build(self):
        name = android_package.OPENXR_QUEST_DEVELOPMENT_PROFILE
        tools, quest = load_contract(name, ["arm64-v8a"])
        generic_tools, generic = load_contract(android_package.OPENXR_DEVELOPMENT_PROFILE, ["arm64-v8a"])
        self.assertEqual(tools["openxrLoader"], generic_tools["openxrLoader"])
        self.assertEqual(quest["openxrProfile"], generic["openxrProfile"])
        self.assertNotEqual(quest["applicationId"], generic["applicationId"])
        self.assertEqual(quest["permissions"][1:], generic["permissions"][1:])
        with self.assertRaisesRegex(AndroidError, "Unsupported profile/ABI pair"):
            load_contract(name, ["x86_64"])

        feature = android_package.QUEST_PASSTHROUGH_FEATURE
        required = f'<uses-feature android:name="{feature}" android:required="true" />'
        self.assertIn(required, android_package.apk_manifest(quest))
        for profile in (generic, self.profile):
            self.assertNotIn(feature, android_package.apk_manifest(profile))

    def test_badging_rejects_missing_optional_or_cross_profile_passthrough(self):
        _, quest = load_contract(android_package.OPENXR_QUEST_DEVELOPMENT_PROFILE, ["arm64-v8a"])
        _, generic = load_contract(android_package.OPENXR_DEVELOPMENT_PROFILE, ["arm64-v8a"])
        feature = android_package.QUEST_PASSTHROUGH_FEATURE
        required = f"  uses-feature: name='{feature}'\n"
        optional = f"  uses-feature-not-required: name='{feature}'\n"
        for profile, accepted in ((quest, required), (generic, "")):
            identity = (f"package: name='{profile['applicationId']}' versionCode='1' versionName='0.2.0'\n"
                        "minSdkVersion:'29'\ntargetSdkVersion:'36'\n")
            permissions = "".join(f"uses-permission: name='{name}'\n" for name in profile["permissions"])
            badging = identity + permissions + "feature-group: label=''\n"
            verify_manifest(badging + accepted, profile)
            for declared in ({required, optional, "", required + optional} - {accepted}):
                with self.subTest(profile=profile["applicationId"], declared=declared), self.assertRaises(AndroidError):
                    verify_manifest(badging + declared, profile)
            with self.assertRaisesRegex(AndroidError, "alternative hardware feature groups"):
                verify_manifest(badging + accepted + "feature-group: label='alternative'\n", profile)

    def test_openxr_loader_requires_both_archive_and_abi_pins_and_explicit_signing(self):
        abi = "arm64-v8a"
        tools, _ = load_contract(android_package.OPENXR_DEVELOPMENT_PROFILE, [abi])
        image = elf(api=24)
        license_text = b"fixture loader license"
        aar = self.root / "loader.aar"
        with zipfile.ZipFile(aar, "w") as archive:
            archive.writestr(f"jni/{abi}/{android_package.OPENXR_LOADER_NAME}", image)
            archive.writestr("META-INF/LICENSE", license_text)
        read = android_package.read_openxr_loader
        for source, unsigned in ((None, False), (aar, True)):
            with self.subTest(source=source, unsigned=unsigned), self.assertRaisesRegex(AndroidError, "requires signing"):
                read(source, tools, [abi], unsigned)
        with self.assertRaisesRegex(AndroidError, "only accepted"):
            read(aar, self.tools, [abi], False)
        with self.assertRaisesRegex(AndroidError, "AAR digest mismatch"):
            read(aar, tools, [abi], False)
        lock = tools["openxrLoader"]
        lock["sha256"] = hashlib.sha256(aar.read_bytes()).hexdigest()
        with self.assertRaisesRegex(AndroidError, f"digest mismatch for {abi}"):
            read(aar, tools, [abi], False)
        lock["abiSha256"][abi] = hashlib.sha256(image).hexdigest()
        with self.assertRaisesRegex(AndroidError, "license digest mismatch"):
            read(aar, tools, [abi], False)
        lock["licenseSha256"] = hashlib.sha256(license_text).hexdigest()
        self.assertEqual(read(aar, tools, [abi], False), android_package.AndroidOpenXRLoader({abi: image}, license_text))

    def test_final_apk_rejects_loader_digest_substitution_even_with_matching_provenance(self):
        abi = "arm64-v8a"
        tools, profile = load_contract(android_package.OPENXR_DEVELOPMENT_PROFILE, [abi])
        locked = tools["openxrLoader"]["abiSha256"]
        image = elf(api=24)
        record = {"name": android_package.OPENXR_LOADER_NAME, "sha256": hashlib.sha256(image).hexdigest()}
        evidence = {"abis": {abi: [record]}, "assets": [], "profile": profile, "toolchain": tools}
        package = self.root / "openxr.apk"
        with zipfile.ZipFile(package, "w") as archive:
            archive.writestr("AndroidManifest.xml", b"manifest")
            archive.writestr("classes.dex", b"dex")
            archive.writestr("assets/horo-package-provenance.json", canonical_json(evidence))
            archive.writestr(f"lib/{abi}/{record['name']}", image)
        with self.assertRaisesRegex(AndroidError, "differs from the locked"):
            inspect_package(package, evidence)
        locked[abi] = record["sha256"]
        with zipfile.ZipFile(package) as archive:
            android_package.verify_native_contents(archive, evidence)
            with self.assertRaisesRegex(AndroidError, "differs from the locked license"):
                android_package.verify_assets(archive, evidence)

    def test_provenance_binds_exact_runtime_dependency_lock(self):
        with patch.object(android_package, "version_output", return_value="abc123"):
            evidence = android_package.provenance(self.tools, self.profile, {}, [])
        lock = android_package.ROOT / "android/gradle/gradle.lockfile"
        self.assertEqual(evidence["javaDependencyLockSha256"], hashlib.sha256(lock.read_bytes()).hexdigest())

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

    def test_portable_paths_reject_traversal_and_absolute(self):
        self.assertEqual(relative_name("assets/ü file.json"), "assets/ü file.json")
        for name in ("../bad", "/bad", "a/../b", "a//b", "C:/bad", "a\\bad"):
            with self.subTest(name=name), self.assertRaises(AndroidError):
                relative_name(name)

    def test_input_path_rejects_links(self):
        source = self.root / "ordinary"
        source.write_bytes(b"ok")
        try:
            (self.root / "link").symlink_to(source)
        except OSError as error:
            if getattr(error, "winerror", None) == 1314:
                self.skipTest("Creating symlinks requires Windows symlink privilege")
            raise
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
        with zipfile.ZipFile(source, "a") as archive:
            archive.writestr("resources.arsc", b"resource table", compress_type=zipfile.ZIP_DEFLATED)
        first, second = self.root / "first.apk", self.root / "second.apk"
        canonical_apk(source, first)
        canonical_apk(source, second)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        with zipfile.ZipFile(first) as archive:
            self.assertEqual(archive.getinfo("lib/arm64-v8a/libentry.so").compress_type, zipfile.ZIP_STORED)
            self.assertEqual(archive.getinfo("resources.arsc").compress_type, zipfile.ZIP_STORED)

    def test_inspection_rejects_compressed_android_resource_tables(self):
        evidence, native = self.evidence()
        source = self.root / "compressed-resources.apk"
        self.package(source, evidence, native)
        with zipfile.ZipFile(source, "a") as archive:
            archive.writestr("resources.arsc", b"resource table", compress_type=zipfile.ZIP_DEFLATED)
        with self.assertRaisesRegex(AndroidError, "resource tables must be uncompressed"):
            inspect_package(source, evidence)
        canonical = self.root / "uncompressed-resources.apk"
        canonical_apk(source, canonical)
        self.assertEqual(len(inspect_package(canonical, evidence)["contents"]), 5)

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
                "uses-permission: name='org.horocore.packagequalification.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION'\n"
                "uses-permission: name='android.permission.ACTIVITY_RECOGNITION'\n")
        verify_manifest(text, self.profile)
        for changed in (text.replace("'29'", "'30'"), text + "uses-permission: name='android.permission.CAMERA'\n",
                        text + "uses-feature: name='android.hardware.vulkan.level'\n"):
            with self.assertRaises(AndroidError):
                verify_manifest(changed, self.profile)

    def test_current_aapt2_min_sdk_badging_and_ambiguous_alias(self):
        text = ("package: name='org.horocore.packagequalification' versionCode='1' versionName='0.2.0'\n"
                "minSdkVersion:'29'\ntargetSdkVersion:'36'\n"
                "uses-permission: name='org.horocore.packagequalification.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION'\n"
                "uses-permission: name='android.permission.ACTIVITY_RECOGNITION'\n")
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
                                  output=self.root / "output", native_root=self.root / "prebuilt", unsigned=True,
                                  openxr_loader=None)

    def badging(self):
        return ("package: name='org.horocore.packagequalification' versionCode='1' versionName='0.2.0'\n"
                "minSdkVersion:'29'\ntargetSdkVersion:'36'\n"
                "uses-permission: name='org.horocore.packagequalification.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION'\n"
                "uses-permission: name='android.permission.ACTIVITY_RECOGNITION'\n")

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
             patch.object(android_package, "android_run", side_effect=self.command), \
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

    def test_openxr_signed_assembly_stages_pinned_abi_loader_in_the_real_package_pipeline(self):
        arguments = self.arguments()
        arguments.profile = android_package.OPENXR_DEVELOPMENT_PROFILE
        arguments.unsigned = False
        arguments.openxr_loader = self.root / "openxr-loader.aar"
        abi = arguments.abi[0]
        image = elf(api=24)
        license_text = b"fixture loader license"
        loader_name = android_package.OPENXR_LOADER_NAME
        with zipfile.ZipFile(arguments.openxr_loader, "w") as archive:
            archive.writestr(f"jni/{abi}/{loader_name}", image)
            archive.writestr("META-INF/LICENSE", license_text)
        tools = self.prepare(arguments)
        native_root = arguments.native_root
        arguments.native_root = None
        locked, profile = load_contract(arguments.profile, arguments.abi)
        tools["openxrLoader"] = dict(locked["openxrLoader"],
                                     sha256=android_package.digest(arguments.openxr_loader),
                                     licenseSha256=hashlib.sha256(license_text).hexdigest(),
                                     abiSha256={abi: hashlib.sha256(image).hexdigest()})
        badging = self.badging().replace(self.profile["applicationId"], profile["applicationId"])
        badging += "".join(f"uses-permission: name='{name}'\n" for name in android_package.OPENXR_PERMISSIONS)
        capability = {"commands": {name: name for name in ("zipalign", "apksigner", "aapt2")},
                      "signing": {"keystore": "external.p12", "alias": "qualification"}}
        with patch.object(android_package, "load_contract", return_value=(tools, profile)), \
             patch.object(android_package, "tools_preflight", return_value=capability), \
             patch.object(android_package, "build_native", return_value=native_root), \
             patch.object(android_package, "android_run", side_effect=self.command), \
             patch.object(android_package, "version_output", side_effect=lambda argv: badging if "badging" in argv else "abc123"):
            android_package.assemble(arguments)
        output = arguments.output
        inspection = json.loads((output / "inspection.json").read_text())
        evidence = json.loads((output / "provenance.json").read_text())
        self.assertTrue(inspection["signed"])
        self.assertEqual(evidence["profile"]["openxrProfile"]["activityRoute"], "cold-discovery")
        with zipfile.ZipFile(output / "signed.apk") as archive:
            entry = archive.getinfo(f"lib/{abi}/{loader_name}")
            self.assertEqual(entry.compress_type, zipfile.ZIP_STORED)
            self.assertEqual(archive.read(entry), image)
            self.assertEqual(archive.read("assets/" + android_package.OPENXR_LOADER_LICENSE), license_text)
        self.assertFalse((native_root / abi / loader_name).exists())

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

    def test_hosted_gradle_cache_reuses_debug_graph_and_isolates_release_resolution(self):
        workflow = (android_package.ROOT / ".github/workflows/android-package.yml").read_text()
        cache_step = workflow.split("- name: Cache Gradle dependencies by build variant", 1)[1]
        cache_step = cache_step.split("- name:", 1)[0]
        self.assertIn("uses: actions/cache@", cache_step)
        cache_paths, key = cache_step.split("key:", 1)
        self.assertEqual(set(cache_paths.split("path: |", 1)[1].split()), {
            "~/.gradle/caches/modules-2/files-2.1",
            "~/.gradle/caches/modules-2/metadata-*",
        })
        self.assertIn("matrix.profile == 'qualification-release' && 'qualification-release' || 'qualification-debug'", cache_paths)
        prefix = "android-gradle-${{ runner.os }}-${{ runner.arch }}-${{ env.GRADLE_CACHE_VARIANT }}-"
        cache_key, restore_keys = key.split("restore-keys: |", 1)
        self.assertTrue(cache_key.strip().startswith(prefix + "locked-"))
        self.assertEqual([line.strip() for line in restore_keys.splitlines() if line.strip()], [prefix + "locked-", prefix])
        hashed_files = key.split("hashFiles(", 1)[1].split(")", 1)[0]
        self.assertEqual({path.strip().strip("'") for path in hashed_files.split(",")}, {
            "android/toolchain-lock.json",
            "android/gradle/build.gradle",
            "android/gradle/settings.gradle",
            "android/gradle/gradle.properties",
            "android/gradle/gradle.lockfile",
        })

    def test_hosted_openxr_cache_contains_only_the_verified_locked_archive(self):
        workflow = (android_package.ROOT / ".github/workflows/android-package.yml").read_text()
        cache = workflow.split("- name: Restore locked OpenXR loader", 1)[1]
        cache = cache.split("- name: Create external disposable qualification signing identity", 1)[0]
        self.assertEqual(cache.count("${{ runner.temp }}/openxr-loader.aar"), 2)
        self.assertEqual(cache.count("hashFiles('android/openxr-loader-lock.json')"), 2)
        self.assertIn('if [[ -f "$loader" ]] && verified "$loader"', cache)
        self.assertIn('if ! verified "$pending"', cache)
        self.assertLess(cache.index('if ! verified "$pending"'), cache.index('mv -- "$pending" "$loader"'))
        self.assertNotIn("restore-keys:", cache)
        self.assertNotIn("qualification.p12", cache)
        build = workflow.split("- name: Build two isolated signed packages and compare evidence", 1)[1]
        self.assertIn('--openxr-loader "$RUNNER_TEMP/openxr-loader.aar"', build)
        self.assertNotIn("curl ", build)

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

    def test_ndk_shared_runtime_uses_exact_host_and_abi(self):
        hosts = {"Linux": "linux-x86_64", "Darwin": "darwin-x86_64", "Windows": "windows-x86_64"}
        triples = {"arm64-v8a": "aarch64-linux-android", "x86_64": "x86_64-linux-android"}
        for host, tag in hosts.items():
            for abi, triple in triples.items():
                with self.subTest(host=host, abi=abi):
                    runtime = self.root / f"toolchains/llvm/prebuilt/{tag}/sysroot/usr/lib/{triple}/libc++_shared.so"
                    runtime.parent.mkdir(parents=True, exist_ok=True)
                    runtime.write_bytes(elf())
                    with patch.object(android_package.host_platform, "system", return_value=host):
                        self.assertEqual(android_package.ndk_shared_runtime(self.root, abi), runtime)

    def test_ndk_shared_runtime_rejects_foreign_host_and_missing_runtime(self):
        foreign = self.root / "toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
        foreign.parent.mkdir(parents=True)
        foreign.write_bytes(elf())
        with patch.object(android_package.host_platform, "system", return_value="Plan9"), self.assertRaises(AndroidError):
            android_package.ndk_shared_runtime(self.root, "arm64-v8a")
        with patch.object(android_package.host_platform, "system", return_value="Darwin"):
            with self.assertRaises(AndroidError):
                android_package.ndk_shared_runtime(self.root, "armeabi-v7a")
            with self.assertRaises(AndroidError):
                android_package.ndk_shared_runtime(self.root, "arm64-v8a")

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
        with patch.dict(os.environ,{},clear=False), patch.object(android_package,"android_run",side_effect=build), \
             patch.object(android_package.host_platform, "system", return_value="Linux"):
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

    def test_staged_activity_includes_real_private_input_permission_bridge(self):
        arguments = self.arguments()
        tools = self.prepare(arguments)
        stage = self.root / "java-input-stage"
        stage.mkdir()
        android_package.apk_write_project(stage, tools, self.profile, ["arm64-v8a"], arguments.game_activity)
        activity = stage / "java/org/horocore/platform/HoroInputActivity.java"
        input_source = android_package.ROOT / "apps/android-input/java/org/horocore/platform/HoroInputActivity.java"
        self.assertEqual(activity.read_bytes(), input_source.read_bytes())
        entry = stage / "java/org/horocore/packagequalification/PackageActivity.java"
        source = entry.read_text()
        self.assertIn("extends org.horocore.platform.HoroInputActivity", source)
        self.assertIn(f'intent.removeExtra("{android_package.DEPLOY_LAUNCH_EXTRA}")', source)
        self.assertIn("firstActivity && state == null", source)
        self.assertIn(android_package.DEPLOY_LAUNCH_MARKER, source)
        self.assertIn("android.permission.ACTIVITY_RECOGNITION", (stage / "AndroidManifest.xml").read_text())

    def test_staged_platform_observation_is_private_to_the_vulkan_profile(self):
        arguments = self.arguments()
        tools = self.prepare(arguments)
        paths = [Path("java/org/horocore/qualification") / name for name in
                 ("HoroVulkanQualificationActivity.java", "PowerObservation.java", "DisplayRefreshObservation.java",
                  "MemoryObservation.java")]
        crash_observer = Path("java/org/horocore/qualification/OpenXRLaunchCrashObserver.java")
        for renderer_profile in ("qualification-vulkan-development", android_package.OPENXR_DEVELOPMENT_PROFILE):
            with self.subTest(profile=renderer_profile):
                _, profile = load_contract(renderer_profile, ["arm64-v8a"])
                stage = self.root / renderer_profile
                stage.mkdir()
                android_package.apk_write_project(stage, tools, profile, ["arm64-v8a"], arguments.game_activity)
                entry = stage / "java" / Path(*profile["applicationId"].split(".")) / "PackageActivity.java"
                if profile["renderer"] == "vulkan-android":
                    for path in paths:
                        source = android_package.ROOT / "apps/android-package-qualification" / path
                        self.assertEqual((stage / path).read_bytes(), source.read_bytes())
                    self.assertIn("extends org.horocore.qualification.HoroVulkanQualificationActivity", entry.read_text())
                    self.assertFalse((stage / crash_observer).exists())
                    self.assertNotIn(android_package.DEPLOY_DEADLINE_EXTRA, entry.read_text())
                else:
                    self.assertTrue(all(not (stage / path).exists() for path in paths))
                    self.assertIn("extends org.horocore.platform.HoroInputActivity", entry.read_text())
                    self.assertEqual((stage / crash_observer).read_bytes(),
                                     (android_package.ROOT / "apps/android-package-qualification" / crash_observer).read_bytes())
                    self.assertIn("OpenXRLaunchCrashObserver.install(nonce, started)", entry.read_text())
                    source = entry.read_text()
                    self.assertIn(f'intent.removeExtra("{android_package.DEPLOY_DEADLINE_EXTRA}")', source)
                    self.assertIn(android_package.DEPLOY_DEADLINE_MARKER, source)
                    self.assertIn("finishAndRemoveTask()", source)
                    self.assertIn("if (diagnosticExpiresAt != 0L) scheduleDiagnosticEnd()", source)


if __name__ == "__main__":
    unittest.main()
