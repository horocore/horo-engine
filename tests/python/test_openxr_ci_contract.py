"""Static CI qualification contracts; never configure or compile a native target."""

import json
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


class OpenXRCIContractTests(unittest.TestCase):
    def setUp(self):
        self.presets = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
        self.workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")

    def preset(self, kind, name):
        return next(value for value in self.presets[kind] if value["name"] == name)

    def variables(self, name):
        preset = self.preset("configurePresets", name)
        parent = preset.get("inherits")
        values = self.variables(parent) if parent else {}
        return values | preset.get("cacheVariables", {})

    def test_enabled_qualification_builds_all_native_and_boundary_targets(self):
        values = self.variables("ci-xr-openxr")
        self.assertEqual(values["HORO_BUILD_XR_OPENXR"], "ON")
        self.assertEqual(values["BUILD_TESTING"], "ON")
        for option in ("HORO_BUILD_EDITOR_GUI", "HORO_BUILD_INPUT_SDL3", "HORO_BUILD_RENDER_OPENGL",
                       "HORO_BUILD_RENDER_METAL", "HORO_BUILD_PHYSICS_NATIVE",
                       "HORO_BUILD_NAVIGATION_RECAST_DETOUR", "HORO_BUILD_NETWORK_GNS"):
            self.assertEqual(values[option], "OFF")
        build = self.preset("buildPresets", "ci-xr-openxr")
        self.assertEqual(set(build["targets"]), {
            "HoroXROpenXRTests", "HoroXROpenXRHostInterfaceConsumer", "HoroXRApiTests",
            "HoroXRRuntimeTests", "HoroXRApiPublicHeaderConsumer", "HoroXRRuntimePublicHeaderConsumer",
            "HoroXRActionBindingTests", "HoroXRInputBindingsPublicHeaderConsumer",
        })
        self.assertEqual(build["jobs"], 2)

    def test_case_inventory_is_selected_serially_without_empty_success(self):
        test = self.preset("testPresets", "ci-xr-openxr")
        selection = test["filter"]["include"]["name"]
        for target in ("HoroXROpenXRTests", "HoroXRApiTests", "HoroXRRuntimeTests", "HoroXRActionBindingTests"):
            self.assertRegex(f"{target}::case", selection)
        self.assertEqual(test["execution"]["jobs"], 1)
        self.assertEqual(self.preset("testPresets", "ci-test-base")["execution"]["noTestsAction"], "error")
        self.assertEqual(self.variables("sonar")["HORO_BUILD_XR_OPENXR"], "ON")

    def test_android_loader_bootstrap_remains_private_and_precedes_native_discovery(self):
        runtime = ROOT / "src/runtime/xr/openxr"
        bindings = (runtime / "host/OpenXRHostBindings.h").read_text(encoding="utf-8")
        self.assertRegex(bindings, r'#if defined\(__ANDROID__\)\s*#include "OpenXRAndroidDiscovery.h"')
        owner = (runtime / "host/OpenXRNativeSession.h").read_text(encoding="utf-8")
        self.assertLess(owner.index("NativeOpenXRAndroidApplication androidApplication_"),
                        owner.index("std::shared_ptr<const Platform::DynamicLibrary> loader_;"))
        session = (runtime / "OpenXRNativeSession.cpp").read_text(encoding="utf-8")
        initializer = session.split("OpenXRNativeSession::InitializeAndroidLoader(", 1)[1].split("#endif", 1)[0]
        self.assertIn("InitializeAndroidOpenXRLoader(dispatch_.getProc, application)", initializer)
        bootstrap = (runtime / "OpenXRAndroidDiscovery.cpp").read_text(encoding="utf-8")
        self.assertIn('Resolve(dispatch, XR_NULL_HANDLE, "xrInitializeLoaderKHR"', bootstrap)
        self.assertIn("XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR", bootstrap)
        self.assertIn("application.applicationContext", bootstrap)
        self.assertIn("extension.required && !extension.promotedToCore", initializer)
        global_dispatch = session.split("OpenXRNativeSession::ResolveGlobal(", 1)[1].split("\n    }", 1)[0]
        self.assertLess(global_dispatch.index("InitializeAndroidLoader(request.extensions)"),
                        global_dispatch.index('"xrEnumerateApiLayerProperties"'))

    def test_android_instance_chain_uses_the_activity_after_enabled_extension_evidence(self):
        runtime = ROOT / "src/runtime/xr/openxr"
        preparation = (runtime / "OpenXRNativePreparation.cpp").read_text(encoding="utf-8")
        transaction = preparation.split("OpenXRNativeSession::PrepareDiscoveryTransaction(", 1)[1].split("\n    }", 1)[0]
        self.assertLess(transaction.index("DiscoverAndNegotiate(request)"), transaction.index("CreateInstance(request)"))
        session = (runtime / "OpenXRNativeSession.cpp").read_text(encoding="utf-8")
        create = session.split("OpenXRNativeSession::CreateInstance(", 1)[1].split("\n    }", 1)[0]
        self.assertIn("ExtensionSlot(AndroidOpenXRInstanceExtension)", create)
        self.assertLess(create.index("NativeExtensionState::Enabled"),
                        create.index("CreateAndroidOpenXRInstance(dispatch_.createInstance, info, androidApplication_, candidate)"))
        bootstrap = (runtime / "OpenXRAndroidDiscovery.cpp").read_text(encoding="utf-8")
        self.assertIn("XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR, info.next, application.vm", bootstrap)
        self.assertIn("application.activity", bootstrap)
        self.assertLess(bootstrap.index("request.next = &activity"), bootstrap.index("return create(&request, &instance)"))

    def test_game_activity_jni_owner_is_built_and_used_by_the_existing_android_host(self):
        platform = ROOT / "src/platform/android"
        package = (ROOT / "cmake/HoroAndroidPackageQualification.cmake").read_text(encoding="utf-8")
        self.assertIn("/src/platform/android/AndroidGameActivityJni.cpp", package)
        entry = (ROOT / "apps/android-package-qualification/NativeEntry.cpp").read_text(encoding="utf-8")
        self.assertIn("GameActivityInput input{*app", entry)
        input_bindings = (platform / "AndroidGameActivityInput.cpp").read_text(encoding="utf-8")
        self.assertIn("jni_.Environment()", input_bindings)
        self.assertNotIn("AttachCurrentThread", input_bindings)
        self.assertNotIn("AcquireGameActivityJniApplication", entry)

    def test_game_activity_producer_keeps_the_context_and_activity_independently_owned(self):
        platform = ROOT / "src/platform/android"
        header = (platform / "AndroidGameActivityJni.h").read_text(encoding="utf-8")
        self.assertIn("GameActivityGlobalRef applicationContext;", header)
        self.assertIn("GameActivityGlobalRef activity;", header)
        source = (platform / "AndroidGameActivityJni.cpp").read_text(encoding="utf-8")
        acquire = source.split("AcquireGameActivityJniApplication(const android_app &app)", 1)[1]
        self.assertIn("app.destroyRequested", acquire)
        self.assertIn("FindApplicationContext(*env, activity)", acquire)
        self.assertIn('getApplicationContext", "()Landroid/content/Context;"', source)
        self.assertLess(acquire.index("PromoteReference(*env, vm, context.get())"),
                        acquire.index("PromoteReference(*env, vm, activity)"))
        self.assertIn("env.NewGlobalRef(reference)", source)
        self.assertIn("env->DeleteGlobalRef(reference)", source)

    def test_android_loader_maps_same_apk_descriptor_after_exact_image_admission(self):
        path = ROOT / "src/platform/android/AndroidApkOpenXRLoader.cpp"
        source = path.read_text(encoding="utf-8")
        owner = source.split("class AndroidApkDynamicLibrary", 1)[1].split("}  // namespace", 1)[0]
        self.assertLess(owner.index("GameActivityGlobalRef context_;"), owner.index("LibraryHandle handle_;"))
        self.assertIn('GetMethodID(type, "getPackageCodePath", "()Ljava/lang/String;")', source)
        self.assertIn("mz_zip_validate_file(&archive, *found, MZ_ZIP_FLAG_VALIDATE_HEADERS_ONLY)", source)
        load = source.split("LoadBundledOpenXRLoader(const GameActivityJniApplication &application,", 1)[1]
        self.assertLess(load.index("MatchesDigest(apk, *offset, entry->m_uncomp_size, imageDigest)"),
                        load.index("android_dlopen_ext"))
        self.assertLess(load.index("options.library_fd = apk.descriptor"), load.index("android_dlopen_ext"))
        self.assertLess(load.index("options.library_fd_offset"), load.index("android_dlopen_ext"))
        self.assertIn("ANDROID_DLEXT_USE_LIBRARY_FD | ANDROID_DLEXT_USE_LIBRARY_FD_OFFSET", load)

    def test_android_cold_discovery_keeps_activity_through_instance_retirement(self):
        runtime = ROOT / "src/runtime/xr/openxr"
        source = (runtime / "OpenXRAndroidDiscovery.cpp").read_text(encoding="utf-8")
        constructor = (runtime / "host/OpenXRAndroidDiscovery.h").read_text(encoding="utf-8")
        self.assertLess(constructor.index("application_;"), constructor.index("loader_;"))
        self.assertIn("XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY", source)
        self.assertNotIn("xrCreateSession", source)
        discover = source.split("AndroidOpenXRColdDiscovery::Discover(", 1)[1]
        self.assertLess(discover.index('loader_->GetSymbol("xrDestroyInstance")'),
                        discover.index("CreateAndroidOpenXRInstance(create"))
        self.assertLess(discover.index("InitializeAndroidOpenXRLoader(dispatch, application_)"),
                        discover.index("AdmitAndroidInstanceExtension(dispatch)"))
        self.assertLess(discover.index("AdmitAndroidInstanceExtension(dispatch)"),
                        discover.index("CreateAndroidOpenXRInstance(create"))
        probe, retirement = source.split("AndroidOpenXRColdDiscovery::Probe(", 1)[1].split(
            "AndroidOpenXRColdDiscovery::Retire(", 1)
        self.assertLess(probe.index("Retire()"), probe.index("return retired == XR_SUCCESS"))
        self.assertIn("AndroidOpenXRProbeResult{InstanceRetirement, retired}", probe)
        self.assertIn("if (result == XR_SUCCESS)\n            instance_ = XR_NULL_HANDLE;", retirement)

    def test_android_openxr_preset_wires_only_its_resume_deferred_diagnostic(self):
        option = "HORO_ANDROID_QUALIFICATION_OPENXR_DISCOVERY"
        self.assertEqual(self.variables("android-openxr-development")[option], "ON")
        for preset in ("android-debug", "android-release", "android-vulkan-development"):
            self.assertNotIn(option, self.variables(preset))
        package = (ROOT / "cmake/HoroAndroidPackageQualification.cmake").read_text(encoding="utf-8")
        self.assertIn('string(JSON _horo_openxr_digest GET "${_horo_openxr_json}" abiSha256 "${ANDROID_ABI}")', package)
        self.assertIn("/apps/android-package-qualification/OpenXRDiagnosticHost.cpp", package)
        app = ROOT / "apps/android-package-qualification"
        entry = (app / "NativeEntry.cpp").read_text(encoding="utf-8")
        self.assertIn("discovery.Command(command)", entry)
        self.assertIn("discovery.Pump(app)", entry)
        host = (app / "OpenXRDiagnosticHost.cpp").read_text(encoding="utf-8")
        command, pump = host.split("OpenXRDiagnosticHost::Command(", 1)[1].split("OpenXRDiagnosticHost::Pump(", 1)
        self.assertIn("APP_CMD_RESUME", command)
        self.assertIn("APP_CMD_PAUSE", command)
        self.assertNotIn("Probe(app)", command)
        self.assertLess(pump.index("State::Attempted"), pump.index("Probe(app)"))
        probe = host.split("OpenXRDiagnosticHost::Probe(", 1)[1].split("OpenXRDiagnosticHost::Command(", 1)[0]
        retirement = host.split("OpenXRDiagnosticHost::PollRetirement(", 1)[1]
        self.assertIn("discovery_ = std::make_unique<AndroidOpenXRColdDiscovery>", probe)
        self.assertIn("if (result.stage != AndroidOpenXRProbeStage::InstanceRetirement)", probe)
        self.assertLess(pump.index("PollRetirement()"), pump.index("State::Queued"))
        self.assertLess(retirement.index("discovery_->Retire()"), retirement.index("discovery_.reset()"))
        self.assertNotIn("Report(", retirement)
        self.assertLess(entry.rindex("Run(*app, collector, owner)"), entry.index("Shutdown(owner.discovery)"))

    def test_off_composition_stays_off_and_checks_real_target_population(self):
        values = self.variables("ci-xr-disabled")
        self.assertEqual(values["HORO_BUILD_XR_OPENXR"], "OFF")
        self.assertEqual(values["HORO_VERIFY_XR_DISABLED"], "ON")
        self.assertEqual(values["BUILD_TESTING"], "OFF")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertRegex(cmake, r'option\(HORO_BUILD_XR_OPENXR\s+"[^"]+"\s+OFF\)')
        policy = (ROOT / "cmake/HoroDependencyPolicy.cmake").read_text(encoding="utf-8")
        guard = re.search(r"if\(HORO_VERIFY_XR_DISABLED\).*?foreach\(target\s+([^)]+)\)", policy, re.S)
        self.assertIsNotNone(guard)
        required = {
            "HoroXROpenXR", "HoroXROpenXRHostInterface", "HoroOpenXRHeaders",
            "HoroXROpenXRVulkan", "HoroXROpenXRVulkanHostInterface", "HoroOpenXRProductHost",
            "HoroOpenXRApplicationBindings", "HoroOpenXRApplicationHost",
        }
        self.assertTrue(required.issubset(set(guard.group(1).split())))
        self.assertIn("FetchContent_GetProperties(horo_openxr_sdk POPULATED", policy)
        for name in ("ci-linux-debug", "ci-macos-debug", "ci-windows-debug"):
            self.assertNotIn("HORO_BUILD_XR_OPENXR", self.variables(name))

    def test_existing_three_platform_matrix_runs_blocking_qualification_and_reports(self):
        for platform in ("Linux / GCC", "macOS / Clang", "Windows / MSVC"):
            self.assertIn(f"name: {platform}", self.workflow)
        for name, command in (
            ("Configure OpenXR native lifecycle qualification", "cmake --preset ci-xr-openxr"),
            ("Build OpenXR lifecycle and header boundaries", "cmake --build --preset ci-xr-openxr"),
            ("Verify OpenXR omitted target graph", "cmake --preset ci-xr-disabled"),
            ("Test OpenXR rollback replacement and shutdown", "ctest --preset ci-xr-openxr"),
        ):
            step = re.search(rf"      - name: {re.escape(name)}\n(.*?)(?=\n      - name:|\Z)", self.workflow, re.S)
            self.assertIsNotNone(step)
            body = step.group(1)
            self.assertIn(command, body)
            self.assertIn("!cancelled()", body)
            self.assertNotIn("continue-on-error", body)
        self.assertIn("steps.xr_build.outcome == 'success'", self.workflow)
        self.assertIn("build/ci*/ctest.xml", self.workflow)


if __name__ == "__main__":
    unittest.main()
