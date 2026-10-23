"""Contracts for CI platform grouping and closed-PR cancellation selection."""
from __future__ import annotations

import json
from pathlib import Path
import re

from ci_gameplay_targets import GAMEPLAY_CHARACTER_TARGETS

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
SUITES = (ROOT / "tests/cmake/HoroCiSuites.cmake").read_text(encoding="utf-8")
PRESETS = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
TERRAIN_RENDER_TARGETS = {"HoroTerrainRenderIntegrationTests", "HoroTerrainRenderIntegrationPublicHeaderConsumer"}


def workflow_job(name: str) -> str:
    match = re.search(rf"^  {re.escape(name)}:\n(.*?)(?=^  [a-z-]+:\n|\Z)", WORKFLOW, re.M | re.S)
    assert match, f"Missing workflow job: {name}"
    return match.group(1)


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


def cmake_conditions_at(source: str, offset: int) -> list[str]:
    conditions = []
    for line in source[:offset].splitlines():
        condition = re.match(r"\s*if\s*\((.*)", line)
        if condition:
            conditions.append(condition.group(1))
        elif re.match(r"\s*endif\s*\(", line):
            conditions.pop()
    return conditions


WINDOWS_NON_AUDIO_TARGETS = frozenset({
    "HoroFoundationJsonTests",
    "HoroWorkspaceLayoutPersistenceTests", "HoroWorkspaceRecoveryPublicHeaderConsumer",
    "HoroNavigationProjectSettingsTests", "HoroNavigationProjectSettingsPublicConsumer",
    "HoroPrefabSceneCookHostTests",
    "HoroPrefabAIGroupTests", "HoroPrefabBuiltinProjectionPublicConsumer",
    "HoroCharacterSceneProductTests", "HoroEditorPlaySessionControllerTests", "HoroProjectPhysicsCaptureBoundaryConsumer",
    "HoroPhysicsTriggerVolumeHostContractConsumer",
    "HoroPhysicsTriggerVolumeHostPublicHeaderConsumer",
    "HoroTriggerVolumeConversionPublicHeaderConsumer",
    "HoroSceneCookPublicHeaderConsumer",
    "HoroPhysicsSceneIntegrationPublicHeaderConsumer",
    "HoroPrefabCookHostPublicHeaderConsumer",
    "HoroWorldStreamingTests", "HoroWorldStreamingPublicHeaderConsumer", "HoroStreamingSourcePlannerBenchmark",
    "HoroDestructionSaveTests", "HoroDestructionSavePublicConsumer",
    "HoroDestructionSaveIntegrationPublicHeaderConsumer", "HoroDestructionApiPublicHeaderConsumer",
    "HoroDestructionReplicationTests", "HoroDestructionReplicationPublicHeaderConsumer",
    "HoroRuntimeSceneCellPayloadTests", "HoroSceneCellPayloadPublicHeaderConsumer",
    "HoroWorldCookIntegrationTests",
    "HoroWorldCookIntegrationConsumer",
    "HoroWorldCookIntegrationPublicHeaderConsumer", "HoroWorldStreamingPublicHeaderConsumer",
    "HoroWorldArtifactCodecTests", "HoroWorldArtifactCodecConsumer",
    "HoroLightCullingTests", "HoroLightFramePoolTests",
    "HoroAITaskSchedulerTests", "HoroAITaskSchedulerPublicConsumer",
    "HoroAIFoundationQualificationTests",
    "HoroGameplayAiTests", "HoroGameplayAiContractConsumer",
    "HoroGameplayAiIntegrationPublicHeaderConsumer", "HoroGameplayAiScriptBridgePublicHeaderConsumer",
    "HoroGameplayApiPublicHeaderConsumer", "HoroGameplayModuleHostPublicHeaderConsumer",
    "HoroAITests", "HoroAISightTests", "HoroAIPublicHeaderConsumer",
    "HoroAISceneIntegrationPublicHeaderConsumer", "HoroAISightIntegrationPublicHeaderConsumer",
    "HoroGameplayPerceptionIntegrationPublicHeaderConsumer",
    "HoroAnimationApiTests", "HoroAnimationGraphPublicHeaderConsumer",
    "HoroD3D12InitializationTests", "HoroD3D12ResourceTests",
    "HoroMaterialBindingTests", "HoroMaterialBindingPublicHeaderConsumer",
    "HoroMultiviewSceneExtractionPublicHeaderConsumer",
    "HoroNetworkDebuggerTests", "HoroNetworkDebuggerPublicHeaderConsumer",
    "HoroPlayTopologyTests", "HoroPlayTopologyPublicHeaderConsumer",
    "HoroTerrainAuthoringTests", "HoroTerrainAuthoringPublicHeaderConsumer",
    "HoroPCGTests", "HoroPCGTerrainAdapterTests",
    "HoroPCGSceneAdapterTests", "HoroPCGSceneAdapterPublicHeaderConsumer",
    "HoroPCGSceneOutputContractConsumer",
    "HoroPCGDocumentTests", "HoroPCGDocumentPublicHeaderConsumer",
    "HoroPCGSceneBakeTests", "HoroPCGSceneBakeContractConsumer", "HoroPCGSceneBakePublicHeaderConsumer", "HoroEditorModelPublicHeaderConsumer",
    "HoroWorldBakeApplicationTests", "HoroWorldBakeApplicationPublicHeaderConsumer",
    "HoroPartitionedWorldWorkspaceTests",
    "HoroInspectorBehaviorBindingTests", "HoroInspectorBehaviorPublicHeaderConsumer",
    "HoroPCGPublicHeaderConsumer", "HoroPCGTerrainAdapterPublicHeaderConsumer",
    "HoroCliCommandRegistryTests", "HoroPlatformTests", "HoroUpdateZipPackageProducerTests",
    "HoroCliOutputPublicHeaderConsumer", "HoroCliProductionOutputContract",
    "HoroCliMcpServeTests", "HoroImmutableFileStorageTests",
    "HoroVfxApiTests", "HoroVfxRenderExtractionTests",
    "HoroVfxApiPublicHeaderConsumer", "HoroRenderApiPublicHeaderConsumer",
    "HoroRenderFrontendPublicHeaderConsumer", "HoroRuntimeScenePublicHeaderConsumer",
    "HoroCinematicModelTests", "HoroCinematicRuntimeTests",
    "HoroFractureDocumentTests", "HoroFractureDocumentPublicHeaderConsumer",
    "HoroCinematicPropertyIntegrationTests", "HoroCinematicModelPublicHeaderConsumer",
    "HoroCinematicRuntimePublicHeaderConsumer", "HoroEditorServicesPublicHeaderConsumer",
    "HoroEditorSurfaceIdentityTests", "HoroSourceFileOpenServiceTests",
    "HoroCameraCutRuntimeTests", "HoroCameraCutPublicHeaderConsumer",
    "HoroPrefabTests", "HoroPrefabSceneExpansionTests", "HoroPrefabSceneExpansionContractConsumer",
    "HoroPrefabOverrideModelPublicHeaderConsumer", "HoroPrefabAuthoringPublicHeaderConsumer",
    "HoroSceneSourceModelPublicHeaderConsumer", "HoroSceneSourcePublicHeaderConsumer",
    "HoroSceneRuntimeConversionPublicHeaderConsumer", "HoroSceneRuntimeConversionTests", "HoroSceneDocumentTests",
    "HoroAssetCookServiceTests", "HoroAssetsPublicHeaderConsumer",
    "HoroAssetRegistryTests", "HoroInputTests", "HoroAndroidInputTests", "HoroAndroidInputIntegrationPublicHeaderConsumer",
    "HoroInputSdlTests", "HoroRuntimeUiInputTests",
    "HoroRuntimeUiInputDebuggerTests", "HoroRuntimeUiInputDebuggerContractConsumer",
    "HoroRuntimeUiAnimationIntegrationTests", "HoroRuntimeUiAnimationIntegrationPublicHeaderConsumer",
    "HoroRuntimeUiHitTestingTests", "HoroRuntimeUiEventDispatchTests", "HoroRuntimeUiPointerPublicHeaderConsumer",
    "HoroCharacterInputTests", "HoroCharacterInputPublicHeaderConsumer",
    "HoroInputPublicHeaderConsumer", "HoroExtensionManagerTests", "HoroMcpSessionTests",
    "HoroEditorActivityBoundaryTests", "HoroExtensionsPublicHeaderConsumer",
    "HoroProjectCodeQueryTests", "HoroProjectReadFilesTests",
    "HoroMcpProjectCodeQueryTests", "HoroSourceDocumentCodeQueryTests",
    "HoroProjectReadFilesApiPublicHeaderConsumer", "HoroPlatformProjectReadFilesPublicHeaderConsumer",
    "HoroProjectCodeQueryPublicHeaderConsumer", "HoroMcpProjectCodeQueryPublicHeaderConsumer",
    "HoroSourceDocumentCodeQueryPublicHeaderConsumer",
    "HoroProjectCollisionSettingsTests", "HoroCollisionSettingsPublicConsumer",
    "HoroPublicationLockFoundationConsumer",
    "HoroProjectCollisionSettingsPublicHeaderConsumer",
    "HoroPackageLifecycleTests", "HoroPackageLifecyclePublicHeaderConsumer",
    "HoroPackageApplicationTests", "HoroPackageApplicationPublicHeaderConsumer",
    "HoroCliPackageAdaptersPublicHeaderConsumer", "HoroMcpPackageAdaptersPublicHeaderConsumer",
    "HoroEditorPackageAdaptersPublicHeaderConsumer",
    "HoroMcpSessionPublicHeaderConsumer", "HoroRuntimeSaveIdentityTests", "HoroRuntimeSaveRootResolverTests",
    "HoroRuntimeSaveStorageQualificationTests", "HoroSaveStorageUserStateQualificationTests",
    "HoroRuntimeSaveFilesystemLockTests", "HoroRuntimeSaveSlotCommitTransactionTests",
    "HoroRuntimeSaveSlotLifecycleTests", "HoroSaveSlotLifecyclePublicHeaderConsumer",
    "HoroRuntimePublicHeaderConsumer", "HoroMixerDocumentTests",
    "HoroRuntimeUiTextLayoutTests", "HoroRuntimeUiTextShapingTests", "HoroRuntimeUiTextUnicodeTests",
    "HoroRuntimeUiUnicodeStartupTests", "HoroRuntimeUiUnicodeLifecycleTests", "HoroRuntimeUiPublicHeaderConsumer",
    "HoroRuntimeUiOverlayLifecycleTests",
    "HoroRuntimeUiHudAssociationTests", "HoroRuntimeUiHudPublicHeaderConsumer",
    "HoroRuntimeUiSceneReconciliationTests", "HoroRuntimeUiSceneReconciliationPublicHeaderConsumer",
    "HoroGameplayRuntimeUiSdkHostConsumer", "HoroRuntimeUiCapabilityTests", "HoroRuntimeUiHotReloadTests", "HoroRuntimeUiHotReloadPublicHeaderConsumer",
    "HoroSceneIdentityPublicHeaderConsumer", "HoroSceneIdentityContractConsumer",
    "HoroRuntimeUiScreenTransitionTests", "HoroRuntimeUiScreenTransitionPublicHeaderConsumer",
    "HoroTerrainSourceArtifactTests", "HoroTerrainSourceArtifactPublicHeaderConsumer",
    "HoroTerrainPayloadManifestTests", "HoroTerrainPayloadManifestPublicHeaderConsumer",
    "HoroTerrainProducerSnapshotTests", "HoroTerrainProducerSnapshotPublicHeaderConsumer",
    "HoroRuntimeSaveEventTriggersTests", "HoroSaveEventTriggersPublicHeaderConsumer",
    "HoroRuntimeSaveRestoreTransactionTests", "HoroSaveGameplayCheckpointPublicHeaderConsumer",
    "HoroNavigationRuntimeTests", "HoroNavigationBakeServiceTests", "HoroNavigationRuntimeQualificationTests",
    "HoroNavigationTransportReservationTests", "HoroNavigationCoordinatorPublicConsumer",
    "HoroNavigationCharacterAdapterTests", "HoroNavigationCharacterAdapterPublicConsumer",
    "HoroNavigationCharacterIntegrationPublicHeaderConsumer",
    "HoroGameplayNavigationTests", "HoroGameplayNavigationPublicConsumer",
    "HoroGameplayNavigationIntegrationPublicHeaderConsumer", "HoroGameplayApiPublicHeaderConsumer",
    "HoroGameplayRuntimePublicHeaderConsumer", "HoroGameplayModuleHostPublicHeaderConsumer",
    "HoroGameplayLuaPublicHeaderConsumer", "HoroNavigationRuntimePublicHeaderConsumer",
})


def test_windows_group_preserves_every_previously_built_target() -> None:
    assert targets("HORO_CI_WINDOWS_TARGETS") == (targets("HORO_CI_AUDIO_TARGETS") | WINDOWS_NON_AUDIO_TARGETS
        | TERRAIN_RENDER_TARGETS | GAMEPLAY_CHARACTER_TARGETS)
    for workflow in ("prefab-foundation-windows", "extension-abi-windows", "mcp-session-windows", "save-path-windows"):
        assert not (ROOT / f".github/workflows/{workflow}.yml").exists()
    assert preset("buildPresets", "ci-windows-debug")["targets"] == ["HoroCiWindowsChecks"]
    assert preset("testPresets", "ci-windows-debug")["filter"]["include"]["label"] == "^ci-windows$"


def test_windows_pointer_gesture_tests_and_consumers_share_build_closure() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    closure = targets("HORO_CI_WINDOWS_TARGETS")
    for target in ("HoroRuntimeUiAnimationIntegrationTests", "HoroRuntimeUiHitTestingTests",
                   "HoroRuntimeUiEventDispatchTests", "HoroInputSdlTests",
                   "HoroRuntimeUiPointerPublicHeaderConsumer", "HoroRuntimeUiAnimationIntegrationPublicHeaderConsumer"):
        assert target in closure, f"Pointer coverage selects {target} without building it"
        assert f"add_executable({target}" in tests_cmake
    assert "unit/runtime/ui/UiPointerInputIntegrationTests.cpp" in tests_cmake
    assert "unit/runtime/ui/UiPointerInteractionTests.cpp" in tests_cmake
    assert "unit/runtime/input/SdlTouchInputTests.cpp" in tests_cmake
    for consumer in ("HoroRuntimeUiPointerPublicHeaderConsumer", "HoroRuntimeUiAnimationIntegrationPublicHeaderConsumer"):
        match = re.search(rf"set_tests_properties\({consumer} PROPERTIES LABELS \"([^\"]+)\"\)", tests_cmake)
        assert match
        assert {"public_headers", "ci-windows"} <= set(match.group(1).split(";"))


def test_windows_light_qualification_has_build_and_discovery_closure() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    registration = re.search(r"set\(HORO_CATCH_TEST_TARGETS\s+(.*?)\n\)", tests_cmake, re.S)
    assert registration, "Missing native Catch registration group"
    for target in ("HoroLightCullingTests", "HoroLightFramePoolTests"):
        assert target in targets("HORO_CI_WINDOWS_TARGETS")
        assert f"add_executable({target}" in tests_cmake
        assert target in registration.group(1).split()
    assert "unit/runtime/renderer/LightFrameBufferPoolTests.cpp" in tests_cmake
    assert "unit/runtime/renderer/LightSceneExtractionTests.cpp" in tests_cmake
    assert 'if(target IN_LIST HORO_CI_WINDOWS_TARGETS)\n        list(APPEND ARG_LABELS ci-windows)' in SUITES






def test_windows_perception_qualification_has_runtime_and_header_build_closure() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    ownership = (ROOT / "cmake/HoroPublicHeaderOwnership.cmake").read_text(encoding="utf-8")
    boundaries = (ROOT / "cmake/HoroTargetBoundaries.cmake").read_text(encoding="utf-8")
    closure = targets("HORO_CI_WINDOWS_TARGETS")
    for target in ("HoroAITests", "HoroAISightTests"):
        assert target in closure
        declaration = tests_cmake.index(f"add_executable({target}")
        assert all("HORO_BUILD_EDITOR_GUI" not in condition
                   for condition in cmake_conditions_at(tests_cmake, declaration))
        assert f"horo_register_catch_test({target} LABELS" in tests_cmake
    assert 'if(target IN_LIST HORO_CI_WINDOWS_TARGETS)\n        list(APPEND ARG_LABELS ci-windows)' in SUITES
    assert 'add_custom_target(HoroCiWindowsChecks DEPENDS ${HORO_CI_WINDOWS_TARGETS})' in SUITES
    direct_tests = re.search(r"set_property\(TEST\s+(.*?)\s+APPEND PROPERTY LABELS ci-windows\)", SUITES, re.S)
    assert direct_tests
    assert 'add_library("${consumer_target}" OBJECT ${generated_sources})' in boundaries
    for owner in ("HoroAI", "HoroAISceneIntegration", "HoroAISightIntegration", "HoroGameplayPerceptionIntegration"):
        consumer = f"{owner}PublicHeaderConsumer"
        assert consumer in closure
        assert consumer not in direct_tests.group(1).split()
        assert f"horo_configure_target_header_boundary({owner} PUBLIC_HEADERS" in ownership
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


def test_ai_foundation_qualification_keeps_its_owner_header_private() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    assert "HoroAIFoundationQualificationTests" in targets("HORO_CI_WINDOWS_TARGETS")
    assert "add_library(HoroAISceneQualificationInternal INTERFACE)" in tests_cmake
    assert '"${horo_ai_qualification_internal_dir}/AISceneRuntimeDetail.h" COPYONLY)' in tests_cmake
    assert '"$<BUILD_INTERFACE:${horo_ai_qualification_internal_dir}>"' in tests_cmake
    assert "target_link_libraries(HoroAISceneQualificationInternal INTERFACE HoroEngine::AISceneIntegration)" in tests_cmake
    assert "target_link_libraries(HoroAIFoundationQualificationTests PRIVATE HoroAISceneQualificationInternal)" in tests_cmake
    for source in ("AIFoundationLifecycleQualificationTests.cpp", "AIFoundationObserverQualificationTests.cpp"):
        assert tests_cmake.count(f"unit/runtime/ai/{source}") == 1
    assert 'horo_register_catch_test(HoroAIFoundationQualificationTests LABELS "unit;ai;headless;qualification")' in tests_cmake


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


def test_windows_character_input_tests_and_public_consumer_share_build_closure() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    ownership = (ROOT / "cmake/HoroPublicHeaderOwnership.cmake").read_text(encoding="utf-8")
    assert {"HoroCharacterInputTests", "HoroCharacterInputPublicHeaderConsumer"} <= targets("HORO_CI_WINDOWS_TARGETS")
    assert "add_executable(HoroCharacterInputTests " in tests_cmake
    assert 'horo_register_catch_test(HoroCharacterInputTests LABELS "unit;character;input;headless")' in tests_cmake
    assert "horo_configure_target_header_boundary(HoroCharacterInput PUBLIC_HEADERS" in ownership
    assert "Horo/CharacterInput/DesiredMotionAdapter.h" in ownership


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


def test_windows_runtime_ui_tests_and_owned_consumers_share_the_build_closure() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    closure = targets("HORO_CI_WINDOWS_TARGETS")
    for target in ("HoroRuntimeUiOverlayLifecycleTests", "HoroRuntimeUiPublicHeaderConsumer",
                   "HoroRuntimeUiHudAssociationTests", "HoroRuntimeUiHudPublicHeaderConsumer"):
        assert target in closure, f"Windows selects {target} without building it"
    assert "add_executable(HoroRuntimeUiOverlayLifecycleTests" in tests_cmake
    assert "add_executable(HoroRuntimeUiHudAssociationTests" in tests_cmake
    assert "add_executable(HoroRuntimeUiHudPublicHeaderConsumer" in tests_cmake
    registration = re.search(r"set\(HORO_CATCH_TEST_TARGETS\s+(.*?)\n\)", tests_cmake, re.S)
    assert registration, "Missing native Catch registration group"
    assert "HoroRuntimeUiOverlayLifecycleTests" in registration.group(1).split()
    assert "HoroRuntimeUiHudAssociationTests" in registration.group(1).split()
    direct_windows = re.search(r"set_property\(TEST\s+(.*?)APPEND PROPERTY LABELS ci-windows\)", SUITES, re.S)
    assert direct_windows
    assert "HoroRuntimeUiHudPublicHeaderConsumer" in direct_windows.group(1).split()
    assert 'target_link_libraries(HoroRuntimeUiHudPublicHeaderConsumer PRIVATE HoroEngine::RuntimeUi)' in tests_cmake
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


def test_windows_source_document_qualification_has_headless_execution_and_consumer_closure() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    registration = re.search(r"set\(HORO_CATCH_TEST_TARGETS\s+(.*?)\n\)", tests_cmake, re.S)
    assert registration, "Missing native Catch registration group"
    source = "HoroSourceFileOpenServiceTests"
    identity = "HoroEditorSurfaceIdentityTests"
    for target in (source, identity):
        assert target in targets("HORO_CI_WINDOWS_TARGETS")
        assert target in registration.group(1).split()
        declaration = tests_cmake.index(f"add_executable({target}")
        assert all("HORO_BUILD_EDITOR_GUI" not in condition
                   for condition in cmake_conditions_at(tests_cmake, declaration))
        assert f"target_link_libraries({target} PRIVATE HoroEngine::EditorServices)" in tests_cmake
    assert tests_cmake.count("unit/editor/SourceDocumentSaveTests.cpp") == 1
    assert "add_library(HoroSourceDocumentPublicHeaderConsumer OBJECT support/SourceDocumentPublicContract.cpp)" in tests_cmake
    assert "target_link_libraries(HoroSourceDocumentPublicHeaderConsumer PRIVATE HoroEngine::EditorServices)" in tests_cmake
    assert f"target_sources({source} PRIVATE $<TARGET_OBJECTS:HoroSourceDocumentPublicHeaderConsumer>)" in tests_cmake
    assert 'horo_register_catch_test(${target} LABELS "native")' in tests_cmake
    assert 'if(target IN_LIST HORO_CI_WINDOWS_TARGETS)\n        list(APPEND ARG_LABELS ci-windows)' in SUITES
    assert 'add_custom_target(HoroCiWindowsChecks DEPENDS ${HORO_CI_WINDOWS_TARGETS})' in SUITES
    assert preset("configurePresets", "ci-windows-debug")["inherits"] == "ci-headless"
    assert preset("configurePresets", "ci-headless")["cacheVariables"]["HORO_BUILD_EDITOR_GUI"] == "OFF"


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
    # Independent native qualification jobs do not change the audio matrix contract.
    audio_job = workflow_job("test")
    for platform in ("Linux / GCC", "macOS / Clang", "Windows / MSVC"):
        assert f"name: {platform}" in audio_job
    assert "Build Release audio checks" in audio_job
    assert "Test Release audio checks" in audio_job
    assert targets("HORO_CI_AUDIO_TARGETS") == {
        "HoroAudioRealtimeSafetyHarnessTests", "HoroAudioWatchdogTests", "HoroAudioNullTests",
        "HoroAudioDspTests", "HoroCoreAudioDspTests", "HoroAudioCommandTests", "HoroAudioMixerTests",
        "HoroAudioEditorPreviewTests", "HoroAudioComponentPreviewHostTests", "HoroAudioFrontendPublicHeaderConsumer",
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


def test_shader_job_keeps_vulkan_native_commands_in_the_same_build() -> None:
    native_job = workflow_job("shaders")
    assert "os: ubuntu-26.04" in native_job
    assert "os: windows-2022" in native_job
    assert "timeout-minutes: 90" in native_job
    assert "-DHORO_BUILD_RENDER_VULKAN=ON" in native_job
    assert "-DHORO_BUILD_EDITOR_GUI=${{ runner.os == 'Linux' && 'ON' || 'OFF' }}" in native_job
    assert "-DHORO_ENABLE_GPU_SMOKE_TESTS=ON" in native_job
    assert "cmake --build build/ci" in native_job
    assert "ctest --test-dir build/ci" in native_job
    build_arguments = native_job.split("cmake --build build/ci", 1)[1].split("      - name:", 1)[0]
    for target in ("HoroRenderVulkanTests", "HoroVulkanCommandExecutionSmoke", "HoroRenderGraphTests",
                   "HoroRenderFrontendTests", "HoroRenderBackendRegistryTests", "HoroXROpenXRTests"):
        assert target in native_job, f"Shader job must build {target}"
        if target != "HoroVulkanCommandExecutionSmoke":
            assert target in build_arguments
    assert "-DHORO_BUILD_XR_OPENXR=ON" in native_job
    assert "--no-tests=error" in native_job
    setup = (ROOT / ".github/actions/setup-build-tools/action.yml").read_text(encoding="utf-8")
    for dependency in ("vulkan-validationlayers", "mesa-vulkan-drivers", "vulkan-tools", "glslang-tools"):
        assert dependency in setup
    assert "scripts/dev.py select-lavapipe-icd" in native_job
    assert "VK_DRIVER_FILES=%s" in native_job
    assert "lvp_icd.x86_64.json" not in native_job
    assert "--output-junit" in native_job
    selection = re.search(r"-R '([^']+)'", native_job)
    assert selection is not None
    selected = re.compile(selection.group(1))
    for target in ("HoroRenderVulkanTests", "HoroRenderGraphTests", "HoroRenderFrontendTests",
                   "HoroRenderBackendRegistryTests", "HoroXROpenXRTests"):
        assert selected.search(f"{target}::representative case [tag]")
        assert not selected.search(f"{target}Unrelated::case")
    assert "-R '^HoroVulkanCommandExecutionSmoke$'" in native_job
    assert not selected.search("UnrelatedTests::case")
    assert "continue-on-error" not in native_job
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    assert "add_test(NAME HoroVulkanCommandExecutionSmoke COMMAND HoroVulkanCommandExecutionSmoke)" in tests_cmake
    assert 'FAIL_REGULAR_EXPRESSION "Validation Error;VUID-"' in tests_cmake


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
    build = re.search(r"        id: mcp_release_build\n(.*?)(?=\n      - name:|\Z)", WORKFLOW, re.S)
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

def test_animation_graph_has_windows_execution_and_sonar_consumer_closure() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    for target in ("HoroAnimationApiTests", "HoroAnimationGraphPublicHeaderConsumer"):
        assert target in targets("HORO_CI_WINDOWS_TARGETS")
        assert tests_cmake.index(f"add_executable({target}") < tests_cmake.index("horo_finalize_ci_suites()")
    assert 'LABELS "unit;animation;headless;public_headers;ci-windows"' in tests_cmake
    for source in ("AnimationGraphTests.cpp", "AnimationGraphBindingTests.cpp", "AnimationGraphSourceTests.cpp"):
        assert f"unit/runtime/animation/{source}" in tests_cmake

def test_windows_save_timeout_diagnostics_preserve_the_original_gate_and_artifacts() -> None:
    def step(name: str) -> str:
        match = re.search(rf"      - name: {re.escape(name)}\n(.*?)(?=\n      - name:|\Z)", WORKFLOW, re.S)
        assert match is not None, f"Missing workflow step: {name}"
        return match.group(1)

    provenance_name = "Record Windows Save allocation diagnostic provenance"
    artifact_name = "Upload Windows Save allocation diagnostics"
    provenance, artifact, debug = step(provenance_name), step(artifact_name), step("Test Debug")
    assert WORKFLOW.index(provenance_name) < WORKFLOW.index("- name: Test Debug")
    assert WORKFLOW.index(artifact_name) > WORKFLOW.index("- name: Test Debug")
    for content in (provenance, artifact):
        assert "runner.os == 'Windows'" in content
        assert "steps.debug_build.outcome == 'success'" in content
    assert "always()" in artifact
    assert "source = (git rev-parse HEAD)" in provenance
    assert "$env:GITHUB_RUN_ID" in provenance
    assert "$env:GITHUB_RUN_ATTEMPT" in provenance
    assert "missing = $true" in provenance
    assert "symbolMatch = 'unverified;" in provenance
    assert "stackCapture = 'not performed;" in provenance
    assert 'run: ctest --preset "${{ matrix.preset }}"' in debug
    assert not any(option in debug for option in ("--timeout", "--repeat", "continue-on-error"))
    windows = preset("testPresets", "ci-windows-debug")
    assert preset("configurePresets", windows["configurePreset"])["binaryDir"] == "${sourceDir}/build/ci"
    assert windows["execution"] == {"jobs": 1, "timeout": 90}
    assert windows["output"]["outputJUnitFile"] == "${sourceDir}/build/ci/ctest.xml"
    assert "build/ci/ctest.xml" in artifact
    assert "build/ci/Testing/Temporary/LastTest.log" in artifact
    for extension in ("exe", "pdb"):
        assert f"build/ci/tests/HoroRuntimeSaveSlotLifecycleTests.{extension}" in provenance
        assert f"build/ci/tests/HoroRuntimeSaveSlotLifecycleTests.{extension}" in artifact
    assert "${{ runner.temp }}/save-allocation-diagnostics/provenance.json" in artifact
    assert "save-allocation-Windows-${{ github.sha }}-${{ github.run_attempt }}" in artifact
    assert "actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a" in artifact


def test_terrain_render_qualification_has_build_discovery_and_compile_consumer_closure() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    test_target = "HoroTerrainRenderIntegrationTests"
    consumer = "HoroTerrainRenderIntegrationPublicHeaderConsumer"
    assert TERRAIN_RENDER_TARGETS <= targets("HORO_CI_WINDOWS_TARGETS")
    assert f"add_executable({test_target}" in tests_cmake
    assert f'horo_register_catch_test({test_target} LABELS "unit;terrain;renderer;headless;lifecycle")' in tests_cmake
    assert f"target_sources({consumer} PRIVATE support/TerrainRenderIntegrationPublicHeaderConsumer.cpp)" in tests_cmake
    boundaries = (ROOT / "cmake/HoroTargetBoundaries.cmake").read_text(encoding="utf-8")
    assert 'add_library("${consumer_target}" OBJECT ${generated_sources})' in boundaries
    ownership = (ROOT / "cmake/HoroPublicHeaderOwnership.cmake").read_text(encoding="utf-8")
    assert "horo_configure_target_header_boundary(HoroTerrainRenderIntegration PUBLIC_HEADERS" in ownership
    direct_tests = re.search(r"set_property\(TEST\s+(.*?)\s+APPEND PROPERTY LABELS ci-windows\)", SUITES, re.S)
    assert direct_tests is not None
    assert consumer not in direct_tests.group(1).split()
    assert 'if(target IN_LIST HORO_CI_WINDOWS_TARGETS)\n        list(APPEND ARG_LABELS ci-windows)' in SUITES


def test_vfx_extraction_has_execution_and_owned_compile_consumer_closure() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    closure = targets("HORO_CI_WINDOWS_TARGETS")
    assert "HoroVfxRenderExtractionTests" in closure
    assert 'horo_register_catch_test(HoroVfxRenderExtractionTests LABELS "unit;vfx;renderer;headless")' in tests_cmake
    assert "unit/runtime/vfx/VfxRenderExtractionTests.cpp" in tests_cmake
    for consumer in ("HoroVfxApiPublicHeaderConsumer", "HoroRenderApiPublicHeaderConsumer",
                     "HoroRenderFrontendPublicHeaderConsumer", "HoroRuntimeScenePublicHeaderConsumer"):
        assert consumer in closure
    boundaries = (ROOT / "cmake/HoroTargetBoundaries.cmake").read_text(encoding="utf-8")
    assert 'add_library("${consumer_target}" OBJECT ${generated_sources})' in boundaries
    assert "support/VfxWorldPublicHeaderConsumer.cpp" in tests_cmake
    assert "support/RenderGraphWorkloadPublicHeaderConsumer.cpp" in tests_cmake


def test_windows_trigger_migration_builds_real_runtime_and_cook_oracles() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    closure = targets("HORO_CI_WINDOWS_TARGETS")
    assert {"HoroPhysicsTests", "HoroPrefabSceneCookHostTests", "HoroRuntimeSceneCellPayloadTests",
            "HoroWorldCookIntegrationTests", "HoroSceneRuntimeConversionTests",
            "HoroPhysicsTriggerVolumeHostContractConsumer", "HoroTriggerVolumeConversionPublicHeaderConsumer"} <= closure
    for source in ("unit/runtime/physics/PhysicsTriggerVolumeRuntimeTests.cpp",
                   "unit/runtime/physics/RuntimeSceneBaselinePhysicsTests.cpp",
                   "unit/application/PhysicsTriggerVolumeHostTests.cpp"):
        assert source in tests_cmake
    for consumer in ("HoroPhysicsTriggerVolumeHostContractConsumer", "HoroTriggerVolumeConversionPublicHeaderConsumer"):
        registration = re.search(rf'set_tests_properties\({consumer} PROPERTIES LABELS "([^"]+)"\)', tests_cmake)
        assert registration is not None
        assert "ci-windows" in registration.group(1).split(";")


def test_vulkan_shader_qualification_keeps_material_packing_and_native_transform_draws() -> None:
    shader_workflow = workflow_job("shaders")
    assert not (ROOT / ".github/workflows/vulkan-shader.yml").exists()
    assert "ubuntu-26.04" in shader_workflow
    assert "windows-2022" in shader_workflow
    flat_targets = {"HoroMaterialBindingPublicHeaderConsumer", "HoroMultiviewSceneExtractionPublicHeaderConsumer"}
    expected_targets = {"HoroRenderVulkanTests", "HoroMaterialBindingTests", "HoroPipelineCacheTests",
                        "HoroShaderPermutationTests", "HoroMultiviewSceneExtractionTests", "HoroXRApiTests",
                        "HoroXROpenXRTests"} | flat_targets
    build_targets = re.search(r"cmake --build build/ci --target\s+(.*?) --parallel 2", shader_workflow, re.S)
    assert build_targets is not None
    expected_targets |= {"HoroRenderGraphTests", "HoroRenderFrontendTests", "HoroRenderBackendRegistryTests",
                         "HoroXROpenXRHostInterfaceConsumer"}
    assert set(build_targets.group(1).split()) == expected_targets
    test_selection = re.search(r"ctest --test-dir build/ci .*?\s+-R '([^']+)'", shader_workflow, re.S)
    assert test_selection is not None
    selected = re.compile(test_selection.group(1))
    for target in expected_targets - flat_targets - {"HoroXROpenXRHostInterfaceConsumer"}:
        assert selected.search(f"{target}::representative case") is not None
        assert selected.search(f"Unrelated{target}::representative case") is None
    for target in flat_targets:
        assert selected.fullmatch(target) is not None
        assert selected.search(f"{target}Extra") is None
    assert "--no-tests=error" in shader_workflow
    assert "VulkanTransform.vert" in shader_workflow
    assert "VulkanTransform.frag" in shader_workflow
    assert "HORO_VULKAN_TRANSFORM_VERTEX" in shader_workflow
    assert "'[native-smoke]'" in shader_workflow
    assert "continue-on-error" not in shader_workflow


def test_vulkan_editor_qualification_uses_locked_cook_and_real_matching_composition() -> None:
    job = workflow_job("shaders")
    assert "HoroEditorVulkanShaderCook --describe-tools" in job
    assert "${archive_digest#sha256:}" in job
    assert "verify-shader-tool" in job
    assert '--relative-path "$executable_relative_path" --sha256 "$executable_digest"' in job
    assert "sha256sum --check" in job
    assert "HORO_EDITOR_VULKAN_ASSET_ROOT" in job
    assert "assets/shaders/editor/viewport.hlsl" in job
    assert "HoroEditorVulkanPresentationSmoke::" in job
    assert "xvfb-run -a ctest" in job
    assert "Vulkan translation units missing from the actual compile database" in job
    assert "Validation (Error|Warning)|VUID-" in job
    assert "catalog.tsv" in job
    assert "HoroEditorAssetRoot-Debug.txt" in job
    apps = (ROOT / "apps/CMakeLists.txt").read_text(encoding="utf-8")
    assert "HoroEditorAssetRoot-$<CONFIG>.txt" in apps
