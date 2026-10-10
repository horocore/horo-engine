"""Contracts for CI platform grouping and closed-PR cancellation selection."""
from __future__ import annotations

import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
SUITES = (ROOT / "tests/cmake/HoroCiSuites.cmake").read_text(encoding="utf-8")
PRESETS = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))


def preset(kind: str, name: str) -> dict:
    return next(item for item in PRESETS[kind] if item["name"] == name)


def targets(name: str) -> set[str]:
    match = re.search(rf"set\({re.escape(name)}\s+(.*?)\n\)", SUITES, re.S)
    assert match, f"Missing CI target group: {name}"
    result = set()
    for value in match.group(1).split():
        if value.startswith("${"):
            result.update(targets(value[2:-1]))
        else:
            result.add(value)
    return result


def test_windows_group_preserves_every_previously_built_target() -> None:
    assert targets("HORO_CI_WINDOWS_TARGETS") == targets("HORO_CI_AUDIO_TARGETS") | {
        "HoroAITaskSchedulerTests", "HoroAITaskSchedulerPublicConsumer",
        "HoroD3D12InitializationTests",
        "HoroMaterialBindingTests", "HoroMaterialBindingPublicHeaderConsumer",
        "HoroNetworkDebuggerTests", "HoroNetworkDebuggerPublicHeaderConsumer",
        "HoroPlayTopologyTests", "HoroPlayTopologyPublicHeaderConsumer",
        "HoroTerrainAuthoringTests", "HoroTerrainAuthoringPublicHeaderConsumer",
        "HoroCliCommandRegistryTests", "HoroPlatformTests", "HoroUpdateZipPackageProducerTests",
        "HoroCliOutputPublicHeaderConsumer", "HoroCliProductionOutputContract",
        "HoroCliMcpServeTests",
        "HoroVfxApiTests", "HoroCinematicModelTests", "HoroCinematicRuntimeTests",
        "HoroFractureDocumentTests", "HoroFractureDocumentPublicHeaderConsumer",
        "HoroCinematicPropertyIntegrationTests", "HoroCinematicModelPublicHeaderConsumer",
        "HoroCinematicRuntimePublicHeaderConsumer", "HoroEditorServicesPublicHeaderConsumer",
        "HoroCameraCutRuntimeTests", "HoroCameraCutPublicHeaderConsumer",
        "HoroPrefabTests", "HoroPrefabSceneExpansionTests", "HoroPrefabSceneExpansionContractConsumer",
        "HoroAssetRegistryTests", "HoroInputTests", "HoroInputSdlTests", "HoroRuntimeUiInputTests",
        "HoroInputPublicHeaderConsumer", "HoroExtensionManagerTests", "HoroMcpSessionTests",
        "HoroEditorActivityBoundaryTests", "HoroExtensionsPublicHeaderConsumer",
        "HoroMcpSessionPublicHeaderConsumer", "HoroRuntimeSaveRootResolverTests",
        "HoroRuntimeSaveStorageQualificationTests", "HoroSaveStorageUserStateQualificationTests",
        "HoroRuntimeSaveFilesystemLockTests", "HoroRuntimeSaveSlotCommitTransactionTests",
        "HoroRuntimeSaveSlotLifecycleTests", "HoroSaveSlotLifecyclePublicHeaderConsumer",
        "HoroRuntimePublicHeaderConsumer", "HoroMixerDocumentTests",
        "HoroRuntimeUiTextLayoutTests", "HoroRuntimeUiTextShapingTests", "HoroRuntimeUiTextUnicodeTests",
        "HoroRuntimeUiUnicodeStartupTests", "HoroRuntimeUiUnicodeLifecycleTests", "HoroRuntimeUiPublicHeaderConsumer",
        "HoroRuntimeUiOverlayLifecycleTests",
        "HoroRuntimeUiScreenTransitionTests", "HoroRuntimeUiScreenTransitionPublicHeaderConsumer",
        "HoroTerrainSourceArtifactTests", "HoroTerrainSourceArtifactPublicHeaderConsumer",
        "HoroTerrainPayloadManifestTests", "HoroTerrainPayloadManifestPublicHeaderConsumer",
        "HoroTerrainProducerSnapshotTests", "HoroTerrainProducerSnapshotPublicHeaderConsumer",
        "HoroRuntimeSaveEventTriggersTests", "HoroSaveEventTriggersPublicHeaderConsumer",
        "HoroRuntimeSaveRestoreTransactionTests", "HoroSaveGameplayCheckpointPublicHeaderConsumer",
        "HoroNavigationRuntimeTests", "HoroNavigationBakeServiceTests",
        "HoroNavigationTransportReservationTests", "HoroNavigationCoordinatorPublicConsumer",
    }
    for workflow in ("prefab-foundation-windows", "extension-abi-windows", "mcp-session-windows", "save-path-windows"):
        assert not (ROOT / f".github/workflows/{workflow}.yml").exists()
    assert preset("buildPresets", "ci-windows-debug")["targets"] == ["HoroCiWindowsChecks"]
    assert preset("testPresets", "ci-windows-debug")["filter"]["include"]["label"] == "^ci-windows$"


def test_ai_scheduler_qualification_has_build_discovery_and_single_source_ownership() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    for target in ("HoroAITaskSchedulerTests", "HoroAITaskSchedulerPublicConsumer"):
        assert target in targets("HORO_CI_WINDOWS_TARGETS")
        assert f"add_executable({target} " in tests_cmake
        assert f"target_link_libraries({target} PRIVATE HoroEngine::AISceneIntegration)" in tests_cmake
    assert tests_cmake.count("unit/runtime/ai/AITaskSchedulerTests.cpp") == 1
    assert 'horo_register_catch_test(HoroAITaskSchedulerTests LABELS "unit;ai;headless")' in tests_cmake
    assert 'add_test(NAME HoroAITaskSchedulerPublicConsumer COMMAND HoroAITaskSchedulerPublicConsumer)' in tests_cmake
    assert 'set_tests_properties(HoroAITaskSchedulerPublicConsumer PROPERTIES LABELS "unit;ai;headless;ci-windows")' in tests_cmake


def test_windows_navigation_qualification_has_build_and_discovery_closure() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    for target in ("HoroNavigationRuntimeTests", "HoroNavigationBakeServiceTests"):
        assert target in targets("HORO_CI_WINDOWS_TARGETS")
        assert f"add_executable({target}" in tests_cmake
    assert "unit/runtime/navigation/NavigationBakeJobsTests.cpp" in tests_cmake
    assert "unit/runtime/navigation/NavigationBakeServiceTests.cpp" in tests_cmake
    assert "unit/runtime/navigation/NavigationBakeQualificationTests.cpp" in tests_cmake
    assert 'horo_register_catch_test(HoroNavigationBakeServiceTests LABELS "unit;navigation;headless;cook")' in tests_cmake
    assert 'horo_register_catch_test(HoroNavigationRuntimeTests LABELS "unit;navigation;headless;lifecycle")' in tests_cmake
    assert 'if(target IN_LIST HORO_CI_WINDOWS_TARGETS)\n        list(APPEND ARG_LABELS ci-windows)' in SUITES
    for name in ("ci-base", "ci-headless", "ci-windows-debug"):
        assert preset("configurePresets", name)["cacheVariables"].get("HORO_BUILD_NAVIGATION_RECAST_DETOUR", "ON") == "ON"
    root_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert 'option(HORO_BUILD_NAVIGATION_RECAST_DETOUR "Build the default grounded navigation query provider" ON)' in root_cmake


def test_navigation_transport_and_coordinator_consumer_have_build_and_execution_closure() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    transport = "HoroNavigationTransportReservationTests"
    consumer = "HoroNavigationCoordinatorPublicConsumer"
    for target in (transport, consumer):
        assert target in targets("HORO_CI_WINDOWS_TARGETS")
        assert f"add_executable({target}" in tests_cmake
    assert f'horo_register_catch_test({transport} LABELS "unit;navigation;headless;concurrency;ci-windows")' in tests_cmake
    assert f"add_test(NAME {consumer} COMMAND {consumer})" in tests_cmake
    windows_labels = re.search(r"set_property\(TEST\s+(.*?)\s+APPEND PROPERTY LABELS ci-windows\)", SUITES, re.S)
    assert windows_labels is not None
    assert consumer in windows_labels.group(1).split()
    assert consumer in targets("HORO_CI_NAVIGATION_TARGETS")
    assert f"set_property(TEST HoroNavigationBakeDiagnosticsPublicConsumer {consumer}\n        APPEND PROPERTY LABELS ci-navigation)" in SUITES


def test_windows_manifest_tests_and_consumer_share_the_build_closure() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    for target in ("HoroTerrainPayloadManifestTests", "HoroTerrainPayloadManifestPublicHeaderConsumer"):
        assert target in targets("HORO_CI_WINDOWS_TARGETS")
        assert f"add_executable({target} " in tests_cmake
    assert 'horo_register_catch_test(HoroTerrainPayloadManifestTests LABELS "unit;terrain;assets;headless")' in tests_cmake
    assert 'if(target IN_LIST HORO_CI_WINDOWS_TARGETS)\n        list(APPEND ARG_LABELS ci-windows)' in SUITES
    assert 'add_test(NAME HoroTerrainPayloadManifestPublicHeaderConsumer COMMAND HoroTerrainPayloadManifestPublicHeaderConsumer)' in tests_cmake
    assert 'set_tests_properties(HoroTerrainPayloadManifestPublicHeaderConsumer PROPERTIES LABELS "unit;terrain;assets;headless;ci-windows")' in tests_cmake
    assert 'add_custom_target(HoroCiWindowsChecks DEPENDS ${HORO_CI_WINDOWS_TARGETS})' in SUITES


def test_windows_producer_snapshot_selection_has_an_executable_build_closure() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    selected_targets = {
        "HoroTerrainProducerSnapshotTests": (
            'horo_register_catch_test(HoroTerrainProducerSnapshotTests '
            'LABELS "unit;terrain;foliage;headless;lifecycle;ci-windows")'
        ),
        "HoroTerrainProducerSnapshotPublicHeaderConsumer": (
            'set_tests_properties(HoroTerrainProducerSnapshotPublicHeaderConsumer '
            'PROPERTIES LABELS "unit;terrain;headless;ci-windows")'
        ),
    }
    build_closure = targets("HORO_CI_WINDOWS_TARGETS")
    for executable, registration in selected_targets.items():
        assert executable in build_closure, f"Windows selects {executable} without building it"
        assert f"add_executable({executable}" in tests_cmake
        assert registration in tests_cmake
    assert 'add_custom_target(HoroCiWindowsChecks DEPENDS ${HORO_CI_WINDOWS_TARGETS})' in SUITES


def test_windows_overlay_tests_and_owned_consumer_share_the_build_closure() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    closure = targets("HORO_CI_WINDOWS_TARGETS")
    for target in ("HoroRuntimeUiOverlayLifecycleTests", "HoroRuntimeUiPublicHeaderConsumer"):
        assert target in closure, f"Windows selects {target} without building it"
    assert "add_executable(HoroRuntimeUiOverlayLifecycleTests" in tests_cmake
    registration = re.search(r"set\(HORO_CATCH_TEST_TARGETS\s+(.*?)\n\)", tests_cmake, re.S)
    assert registration, "Missing native Catch registration group"
    assert "HoroRuntimeUiOverlayLifecycleTests" in registration.group(1).split()
    assert 'foreach (target IN LISTS HORO_CATCH_TEST_TARGETS)' in tests_cmake
    assert 'horo_register_catch_test(${target} LABELS "native")' in tests_cmake
    assert 'target_sources(HoroRuntimeUiPublicHeaderConsumer PRIVATE support/RuntimeUiOverlayPublicContract.cpp)' in tests_cmake
    assert 'if(target IN_LIST HORO_CI_WINDOWS_TARGETS)\n        list(APPEND ARG_LABELS ci-windows)' in SUITES
    assert 'add_custom_target(HoroCiWindowsChecks DEPENDS ${HORO_CI_WINDOWS_TARGETS})' in SUITES


def test_windows_screen_transition_build_and_execution_are_selected() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    suite = "HoroRuntimeUiScreenTransitionTests"
    consumer = "HoroRuntimeUiScreenTransitionPublicHeaderConsumer"
    for target in (suite, consumer):
        assert target in targets("HORO_CI_WINDOWS_TARGETS")
        assert f"add_executable({target}" in tests_cmake
    registration = re.search(r"set\(HORO_CATCH_TEST_TARGETS\s+(.*?)\n\)", tests_cmake, re.S)
    assert registration
    assert suite in registration.group(1).split()
    direct_selection = re.search(r"set_property\(TEST\s+(.*?)\s+APPEND PROPERTY LABELS ci-windows\)", SUITES, re.S)
    assert direct_selection
    assert consumer in direct_selection.group(1).split()
    assert f"add_test(NAME {consumer} COMMAND {consumer})" in tests_cmake


def test_windows_suites_run_independently_and_remain_blocking() -> None:
    # CTest continues through cases by default. Workflow test stages must also
    # remain runnable after another stage fails, and failures must fail the job.
    for name in ("Test Debug", "Test Release audio checks", "Test navigation without Recast Detour",
                 "Test GNS connections, DNS, delivery and lifecycle"):
        match = re.search(rf"      - name: {re.escape(name)}\n(.*?)(?=\n      - name:|\Z)", WORKFLOW, re.S)
        assert match
        assert "!cancelled()" in match.group(1)
        assert "continue-on-error" not in match.group(1)
        assert "--stop-on-failure" not in match.group(1)
    assert preset("testPresets", "ci-test-base")["execution"]["noTestsAction"] == "error"
    for name in ("ci-windows-debug", "ci-audio-release", "ci-navigation-null", "ci-network-gns"):
        assert preset("testPresets", name)["output"]["outputJUnitFile"].endswith("/ctest.xml")
    for name in ("HoroAudioCallbackLockPolicyTest", "HoroPrefabSceneExpansionContractConsumer",
                 "HoroExtensionManagerTests", "HoroExtensionAbiConformanceCliSupported",
                 "HoroExtensionAbiConformanceCliIncompatible", "HoroExtensionAbiConformanceCliRequiresModule"):
        assert name in SUITES


def test_fracture_consumer_extends_the_owned_header_boundary_target() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    ownership = (ROOT / "cmake/HoroPublicHeaderOwnership.cmake").read_text(encoding="utf-8")
    target = "HoroFractureDocumentPublicHeaderConsumer"
    assert "horo_configure_target_header_boundary(HoroFractureDocument PUBLIC_HEADERS" in ownership
    assert tests_cmake.index("horo_add_public_header_consumer_targets()") < tests_cmake.index(f"target_sources({target}")
    assert tests_cmake.count(f"target_sources({target}") == 1
    assert f"add_executable({target}" not in tests_cmake
    assert f"add_library({target}" not in tests_cmake
    assert f"add_test(NAME {target}" not in tests_cmake
    assert target in targets("HORO_CI_WINDOWS_TARGETS")


def test_audio_keeps_both_modes_and_all_platforms() -> None:
    assert set(re.findall(r"^  ([a-z-]+):$", WORKFLOW.split("jobs:\n", 1)[1], re.M)) == {"tooling", "android-storage", "test"}
    for platform in ("Linux / GCC", "macOS / Clang", "Windows / MSVC"):
        assert f"name: {platform}" in WORKFLOW
    assert targets("HORO_CI_AUDIO_TARGETS") == {
        "HoroAudioRealtimeSafetyHarnessTests", "HoroAudioWatchdogTests", "HoroAudioNullTests",
        "HoroAudioDspTests", "HoroCoreAudioDspTests", "HoroAudioCommandTests", "HoroAudioMixerTests",
        "HoroAudioEditorPreviewTests", "HoroAudioFrontendPublicHeaderConsumer",
    }
    assert preset("configurePresets", "ci-native-debug")["cacheVariables"]["CMAKE_BUILD_TYPE"] == "Debug"
    assert preset("configurePresets", "ci-audio-release")["cacheVariables"]["CMAKE_BUILD_TYPE"] == "Release"
    assert preset("buildPresets", "ci-audio-release")["targets"] == ["HoroCiAudioRealtimeChecks"]
    release = preset("testPresets", "ci-audio-release")
    assert release["filter"]["include"]["label"] == "^realtime-safety$"
    assert release["execution"] == {"jobs": 1, "timeout": 90}
    for name in ("ci-linux-debug", "ci-macos-debug"):
        assert "targets" not in preset("buildPresets", name)  # Debug builds audio with all native targets.
        assert preset("testPresets", name)["filter"]["exclude"]["label"] == "gui|tooling"


def test_required_checks_and_sdl_composition_are_preserved() -> None:
    assert "name: Test · ${{ matrix.name }}" in WORKFLOW
    assert "name: Linux / GCC" in WORKFLOW
    assert "name: macOS / Clang" in WORKFLOW
    assert "profile: native-tests-v1" in WORKFLOW
    assert preset("configurePresets", "ci-native-debug")["binaryDir"] == "${sourceDir}/build/ci"
    windows = preset("configurePresets", "ci-windows-debug")
    assert windows["cacheVariables"]["HORO_BUILD_INPUT_SDL3"] == "ON"
    assert windows["inherits"] == "ci-headless"
    headless = preset("configurePresets", "ci-headless")["cacheVariables"]
    assert headless["HORO_BUILD_AUDIO_SDL3"] == "OFF"
    assert headless["HORO_BUILD_EDITOR_GUI"] == "OFF"
    assert not (ROOT / ".github/workflows/ci-network-gns.yml").exists()
    assert targets("HORO_CI_NETWORK_TARGETS") == {"HoroNetworkTransportGnsTests", "HoroNetworkApiPublicHeaderConsumer"}
    assert preset("buildPresets", "ci-network-gns")["targets"] == ["HoroCiNetworkGnsChecks"]
    assert preset("testPresets", "ci-network-gns")["filter"]["include"]["label"] == "^gns$"
    network = preset("configurePresets", "ci-network-base")["cacheVariables"]
    assert network["HORO_BUILD_PHYSICS_NATIVE"] == "OFF"
    assert network["HORO_BUILD_NAVIGATION_RECAST_DETOUR"] == "OFF"
    assert preset("configurePresets", "ci-network-disabled")["cacheVariables"]["HORO_VERIFY_NETWORK_DISABLED"] == "ON"
    sonar = (ROOT / ".github/workflows/sonar.yml").read_text(encoding="utf-8")
    assert "name: SonarCloud" in sonar
    assert "ctest --preset sonar" in sonar
    assert not re.search(r"^\s+build/sonar/tests/", sonar, re.M)
    assert preset("configurePresets", "sonar")["cacheVariables"]["HORO_CI_SONAR_COVERAGE"] == "ON"
    cleanup = (ROOT / ".github/workflows/cancel-closed-pr.yml").read_text(encoding="utf-8")
    assert "pull_request_target:" in cleanup
    assert "types: [closed]" in cleanup
    assert "actions: write" in cleanup
    assert "actions/checkout" not in cleanup
    assert "contents: read" not in cleanup
    assert "await cancelClosedPrRuns" in cleanup


def test_installed_manifest_source_keeps_sonar_coverage() -> None:
    collector = (ROOT / ".github/scripts/collect_sonar_coverage.sh").read_text(encoding="utf-8")
    assert '"$workspace/apps/HoroEditor/app/ConfiguredEditorUpdateManifestSource.cpp"' in collector
    assert "HoroConfiguredEditorUpdateBackendTests" in targets("HORO_SONAR_EDITOR_TARGETS")


def test_instrumented_render_graph_pane_keeps_sonar_coverage() -> None:
    collector = (ROOT / ".github/scripts/collect_sonar_coverage.sh").read_text(encoding="utf-8")
    assert '"$workspace/apps/HoroEditor/app/RenderGraphInspectionPane.cpp"' in collector
    assert '"$workspace/apps/HoroEditor/app/"' not in collector
    test_targets = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    assert "target_sources(HoroGlobalDockPanelRenderTests PRIVATE unit/editor/RenderGraphInspectionPaneTests.cpp" in test_targets
    assert "../apps/HoroEditor/app/RenderGraphInspectionPane.cpp)" in test_targets
    assert preset("configurePresets", "sonar")["inherits"] == "ci-linux-debug"
    assert preset("configurePresets", "ci-linux-debug")["inherits"] == "ci-native-debug"
    assert preset("configurePresets", "ci-native-debug")["cacheVariables"]["HORO_BUILD_EDITOR_GUI"] == "ON"


def test_windows_restore_allocation_sweep_has_mandatory_release_coverage() -> None:
    build = re.search(r"      - name: Build Windows Release MCP and restore allocation qualification\n(.*?)(?=\n      - name:|\Z)", WORKFLOW, re.S)
    assert build
    assert "--target HoroMcpSessionTests HoroRuntimeSaveRestoreTransactionTests" in build.group(1)
    test = re.search(r"      - name: Test Windows Release restore allocation qualification\n(.*?)(?=\n      - name:|\Z)", WORKFLOW, re.S)
    assert test
    assert "!cancelled()" in test.group(1)
    assert "steps.mcp_release_build.outcome == 'success'" in test.group(1)
    assert "--no-tests=error --timeout 90 -R '^HoroRuntimeSaveRestoreTransactionTests::'" in test.group(1)
    assert "continue-on-error" not in test.group(1)
    assert "--output-junit build/ci-audio-release/restore-ctest.xml" in test.group(1)
    assert "            build/ci-audio-release/restore-ctest.xml" in WORKFLOW


def test_windows_material_binding_has_tests_and_owned_consumer() -> None:
    cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    for target in ("HoroMaterialBindingTests", "HoroMaterialBindingPublicHeaderConsumer"):
        assert target in targets("HORO_CI_WINDOWS_TARGETS")
        assert f"add_executable({target}" in cmake
    registry = (ROOT / "cmake/HoroPublicHeaderOwnership.cmake").read_text(encoding="utf-8")
    for header in ("MaterialBinding.h", "MaterialBindingBackend.h", "MaterialBindingErrors.h"):
        assert registry.count(f"Horo/Runtime/Render/{header}") == 1
    assert "        HoroMaterialBindingTests\n" in cmake
