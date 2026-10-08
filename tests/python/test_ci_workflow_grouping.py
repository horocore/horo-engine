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
        "HoroNetworkDebuggerTests", "HoroNetworkDebuggerPublicHeaderConsumer",
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
        "HoroRuntimeSaveFilesystemLockTests", "HoroRuntimeSaveSlotCommitTransactionTests",
        "HoroRuntimePublicHeaderConsumer", "HoroMixerDocumentTests",
        "HoroRuntimeUiTextLayoutTests", "HoroRuntimeUiTextShapingTests", "HoroRuntimeUiTextUnicodeTests",
        "HoroRuntimeUiUnicodeStartupTests", "HoroRuntimeUiUnicodeLifecycleTests", "HoroRuntimeUiPublicHeaderConsumer",
        "HoroTerrainSourceArtifactTests", "HoroTerrainSourceArtifactPublicHeaderConsumer",
        "HoroRuntimeSaveEventTriggersTests", "HoroSaveEventTriggersPublicHeaderConsumer",
    }
    for workflow in ("prefab-foundation-windows", "extension-abi-windows", "mcp-session-windows", "save-path-windows"):
        assert not (ROOT / f".github/workflows/{workflow}.yml").exists()
    assert preset("buildPresets", "ci-windows-debug")["targets"] == ["HoroCiWindowsChecks"]
    assert preset("testPresets", "ci-windows-debug")["filter"]["include"]["label"] == "^ci-windows$"


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
    assert set(re.findall(r"^  ([a-z-]+):$", WORKFLOW.split("jobs:\n", 1)[1], re.M)) == {"tooling", "test"}
    for platform in ("Linux / GCC", "macOS / Clang", "Windows / MSVC"):
        assert f"name: {platform}" in WORKFLOW
    assert targets("HORO_CI_AUDIO_TARGETS") == {
        "HoroAudioRealtimeSafetyHarnessTests", "HoroAudioWatchdogTests", "HoroAudioNullTests",
        "HoroAudioDspTests", "HoroCoreAudioDspTests", "HoroAudioCommandTests", "HoroAudioMixerTests",
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
