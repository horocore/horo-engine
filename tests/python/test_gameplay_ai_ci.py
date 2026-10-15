"""Build, discovery, public ownership and exact SDK identity of the AI capability boundary."""
from __future__ import annotations

from test_ci_workflow_grouping import ROOT, targets


def test_gameplay_ai_native_script_and_consumer_windows_closure() -> None:
    closure = targets("HORO_CI_WINDOWS_TARGETS")
    required = {"HoroGameplayAiTests", "HoroGameplayAiContractConsumer",
                "HoroGameplayAiIntegrationPublicHeaderConsumer", "HoroGameplayAiScriptBridgePublicHeaderConsumer",
                "HoroGameplayApiPublicHeaderConsumer", "HoroGameplayModuleHostPublicHeaderConsumer",
                "HoroAIPublicHeaderConsumer", "HoroAISceneIntegrationPublicHeaderConsumer", "HoroExtensionsPublicHeaderConsumer"}
    assert required <= closure
    source = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    assert "horo_register_catch_test(HoroGameplayAiTests LABELS" in source
    assert "add_dependencies(HoroGameplayAiTests HoroTestAiGameModule)" in source
    assert "add_test(NAME HoroGameplayAiContractConsumer" in source
    for filename in ("GameplayAiCapabilityTests.cpp", "GameplayAiScriptTests.cpp", "GameplayAiHostTests.cpp"):
        assert f"unit/gameplay/{filename}" in source
    ownership = (ROOT / "cmake/HoroPublicHeaderOwnership.cmake").read_text(encoding="utf-8")
    for header in ("Horo/AI/AICapabilityDescriptors.h", "Horo/AI/AIRuntimeCapability.h",
                   "Horo/Gameplay/GameplayAiContext.h", "Horo/Gameplay/GameplayAiScriptBridge.h"):
        assert ownership.count(header) == 1
    for target in ("HoroGameplayAiIntegration", "HoroGameplayAiScriptBridge"):
        assert ownership.index(f"horo_configure_target_header_boundary({target} PUBLIC_HEADERS") < ownership.index(
            "horo_verify_public_header_inventory()")


def test_gameplay_ai_dependency_policy_matches_only_the_real_downstream_edges() -> None:
    policy = (ROOT / "cmake/HoroDependencyPolicy.cmake").read_text(encoding="utf-8")
    assert "TARGET HoroGameplayAiIntegration DEPENDENCIES HoroGameplayApi HoroAISceneIntegration)" in policy
    assert "TARGET HoroGameplayAiScriptBridge DEPENDENCIES HoroGameplayAiIntegration HoroExtensions)" in policy
    host = policy.split("TARGET HoroGameplayModuleHost", 1)[1].split(")", 1)[0].split()
    assert host == ["DEPENDENCIES", "HoroGameplayRuntime", "HoroPlatform",
                    "HoroGameplayPhysicsIntegration", "HoroGameplayCharacterIntegration",
                    "HoroGameplayNavigationIntegration", "HoroGameplayAiIntegration"]


def test_native_ai_module_links_only_position_independent_static_owners() -> None:
    source = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    loop = source.split("foreach(target HoroPrefabRuntime", 1)[1].split("endforeach()", 1)[0]
    owners = loop.split("get_target_property", 1)[0].split(")", 1)[0].split()
    assert {"HoroGameplayAiIntegration", "HoroAISceneIntegration"} <= set(owners)
    assert 'if(target_type STREQUAL "STATIC_LIBRARY")' in loop
    assert "set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)" in loop
    fixture = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    assert "target_link_libraries(HoroTestAiGameModule PRIVATE HoroEngine::GameplayAiIntegration)" in fixture


def test_two_module_qualification_executes_as_a_cross_platform_ctest() -> None:
    tests = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    registration = tests.split("add_test(NAME HoroGameplayModuleInputManifestTest", 1)[1].split("add_test(", 1)[0]
    assert 'COMMAND "${CMAKE_COMMAND}"' in registration
    assert '-P "${PROJECT_SOURCE_DIR}/tests/cmake/RunGameplayModuleInputManifestTest.cmake"' in registration
    assert 'LABELS "gameplay;cmake;ci-windows" TIMEOUT 90' in registration
    runner = (ROOT / "tests/cmake/RunGameplayModuleInputManifestTest.cmake").read_text(encoding="utf-8")
    assert 'file(REMOVE_RECURSE "${HORO_TEST_BINARY_DIR}")' in runner
    assert 'COMMAND "${CMAKE_COMMAND}" -S "${source_dir}" -B "${build_dir}" -G Ninja' in runner
    assert 'if(NOT result EQUAL 0)' in runner
    assert "Project with spaces" in runner
    assert "build with spaces" in runner
