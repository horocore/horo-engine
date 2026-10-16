"""Platform build and discovery closure for Navigation and its Character bridge."""
from __future__ import annotations

import re

from test_ci_workflow_grouping import ROOT, SUITES, WORKFLOW, cmake_conditions_at, preset, targets


def test_navigation_character_bridge_has_platform_build_and_execution_closure() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    ownership = (ROOT / "cmake/HoroPublicHeaderOwnership.cmake").read_text(encoding="utf-8")
    target_source = (ROOT / "src/CMakeLists.txt").read_text(encoding="utf-8")
    bridge_targets = {
        "HoroNavigationCharacterAdapterTests",
        "HoroNavigationCharacterAdapterPublicConsumer",
        "HoroNavigationCharacterIntegrationPublicHeaderConsumer",
    }
    assert bridge_targets <= targets("HORO_CI_WINDOWS_TARGETS")
    assert bridge_targets <= targets("HORO_CI_NAVIGATION_TARGETS")
    for executable in bridge_targets - {"HoroNavigationCharacterIntegrationPublicHeaderConsumer"}:
        declaration = f"add_executable({executable} "
        assert declaration in tests_cmake
        assert not cmake_conditions_at(tests_cmake, tests_cmake.index(declaration)), "Bridge tests must also exist in Null compositions"
    assert 'horo_register_catch_test(HoroNavigationCharacterAdapterTests LABELS "unit;navigation;character;headless;ci-navigation")' in tests_cmake
    assert "if(target IN_LIST HORO_CI_WINDOWS_TARGETS)" in SUITES
    assert "add_test(NAME HoroNavigationCharacterAdapterPublicConsumer COMMAND HoroNavigationCharacterAdapterPublicConsumer)" in tests_cmake
    for label in ("ci-windows", "ci-navigation"):
        direct_tests = re.findall(rf"set_property\(TEST\s+([^)]*?)\s+APPEND PROPERTY LABELS {label}\)", SUITES, re.S)
        assert any("HoroNavigationCharacterAdapterPublicConsumer" in block.split() for block in direct_tests)
    assert "horo_configure_target_header_boundary(HoroNavigationCharacterIntegration PUBLIC_HEADERS" in ownership
    assert "Horo/Navigation/NavigationCharacterAdapter.h" in ownership
    assert "target_link_libraries(HoroNavigationCharacterIntegration PUBLIC HoroEngine::NavigationRuntime HoroEngine::CharacterInput)" in target_source


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


def test_runtime_qualification_builds_real_native_route_and_explicit_omitted_admission() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    target = "HoroNavigationRuntimeQualificationTests"
    assert target in targets("HORO_CI_WINDOWS_TARGETS")
    assert target in targets("HORO_CI_NAVIGATION_TARGETS")
    declaration = f"add_executable({target}"
    assert not cmake_conditions_at(tests_cmake, tests_cmake.index(declaration))
    assert "integration/navigation/NavigationRuntimeQualificationAvailabilityTests.cpp" in tests_cmake
    native_sources = (
        "integration/navigation/NavigationRuntimeQualificationTests.cpp",
        "integration/navigation/NavigationRuntimePressureQualificationTests.cpp",
    )
    for source in native_sources:
        conditions = cmake_conditions_at(tests_cmake, tests_cmake.index(source))
        assert any("HORO_BUILD_NAVIGATION_RECAST_DETOUR AND HORO_BUILD_PHYSICS_NATIVE" in condition for condition in conditions)
    assert f'horo_register_catch_test({target} LABELS "integration;navigation;character;headless;qualification;ci-navigation")' in tests_cmake


def test_runtime_qualification_omitted_profiles_admit_exact_provider_combinations() -> None:
    combinations = {
        "ci-navigation-null": ("OFF", "ON"),
        "ci-navigation-physics-null": ("ON", "OFF"),
        "ci-navigation-providers-null": ("OFF", "OFF"),
    }
    focused_targets = {
        "HoroNavigationRuntimeQualificationTests", "HoroNavigationCharacterAdapterPublicConsumer",
        "HoroNavigationCharacterIntegrationPublicHeaderConsumer", "HoroNavigationCoordinatorPublicConsumer",
        "HoroCharacterInputPublicHeaderConsumer", "HoroPhysicsPublicHeaderConsumer",
    }
    assert preset("testPresets", "ci-test-base")["execution"]["noTestsAction"] == "error"
    for name, (navigation, physics) in combinations.items():
        configuration = preset("configurePresets", name)
        variables = configuration["cacheVariables"]
        assert variables["HORO_BUILD_NAVIGATION_RECAST_DETOUR"] == navigation
        assert variables["HORO_BUILD_PHYSICS_NATIVE"] == physics
        if name == "ci-navigation-null":
            assert "HoroNavigationRuntimeQualificationTests" in targets("HORO_CI_NAVIGATION_TARGETS")
            continue
        assert configuration["inherits"] == "ci-navigation-null"
        assert configuration["binaryDir"] == "${sourceDir}/build/" + name
        assert set(preset("buildPresets", name)["targets"]) == focused_targets
        execution = preset("testPresets", name)
        assert execution["inherits"] == "ci-test-base"
        assert execution["execution"]["jobs"] == 1
        assert execution["execution"]["noTestsAction"] == "error"
        assert execution["output"]["outputJUnitFile"] == "${sourceDir}/build/" + name + "/ctest.xml"
        pattern = execution["filter"]["include"]["name"]
        assert re.search(pattern, "HoroNavigationRuntimeQualificationTests::"
                         "Runtime qualification records actual native provider admission without silent substitution")
        for consumer in ("HoroNavigationCharacterAdapterPublicConsumer", "HoroNavigationCoordinatorPublicConsumer"):
            assert re.search(pattern, consumer)
        for compile_only in ("HoroCharacterInputPublicHeaderConsumer", "HoroNavigationCharacterIntegrationPublicHeaderConsumer",
                             "HoroPhysicsPublicHeaderConsumer"):
            assert not re.search(pattern, compile_only)


def test_runtime_qualification_omitted_profiles_are_built_and_executed_in_ci() -> None:
    profiles = {
        "ci-navigation-physics-null": ("Physics", "navigation_physics_null_build"),
        "ci-navigation-providers-null": ("both native providers", "navigation_providers_null_build"),
    }
    for name, (label, build_id) in profiles.items():
        steps = {}
        for phase in ("Configure", "Build", "Test"):
            title = f"{phase} navigation qualification without {label}"
            match = re.search(rf"      - name: {re.escape(title)}\n(.*?)(?=\n      - name:|\Z)", WORKFLOW, re.S)
            assert match is not None
            steps[phase] = match
            assert "runner.os == 'Linux'" in match.group(1)
        assert f"run: cmake --preset {name}" in steps["Configure"].group(1)
        assert f"id: {build_id}" in steps["Build"].group(1)
        assert f"run: cmake --build --preset {name}" in steps["Build"].group(1)
        assert f"steps.{build_id}.outcome == 'success'" in steps["Test"].group(1)
        assert f"run: ctest --preset {name}" in steps["Test"].group(1)
        assert steps["Configure"].start() < steps["Build"].start() < steps["Test"].start()
    assert "build/ci*/ctest.xml" in WORKFLOW
