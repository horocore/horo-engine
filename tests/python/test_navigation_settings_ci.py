"""Navigation project authoring must execute in headless and desktop CI closures."""
from __future__ import annotations

from test_ci_workflow_grouping import ROOT, cmake_conditions_at, targets


def test_navigation_settings_has_real_build_and_discovery_closure() -> None:
    cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    names = {
        "HoroNavigationProjectSettingsTests",
        "HoroNavigationProjectSettingsPublicConsumer",
    }
    for suite in ("HORO_CI_WINDOWS_TARGETS", "HORO_CI_NAVIGATION_TARGETS", "HORO_SONAR_EDITOR_TARGETS"):
        assert names <= targets(suite)
    for name in names:
        declaration = f"add_executable({name}"
        assert declaration in cmake
        assert not cmake_conditions_at(cmake, cmake.index(declaration))
    assert 'horo_register_catch_test(HoroNavigationProjectSettingsTests LABELS "unit;editor;navigation;headless;ci-navigation")' in cmake
    assert "add_test(NAME HoroNavigationProjectSettingsPublicConsumer COMMAND HoroNavigationProjectSettingsPublicConsumer)" in cmake
    for source in (
        "NavigationProjectSettingsTests.cpp",
        "NavigationProjectSettingsSessionTests.cpp",
        "NavigationSettingsWorkflowTests.cpp",
    ):
        assert source in cmake


def test_navigation_settings_public_headers_have_one_real_owner() -> None:
    ownership = (ROOT / "cmake/HoroPublicHeaderOwnership.cmake").read_text(encoding="utf-8")
    for header in (
        "Horo/Application/NavigationProjectSettings.h",
        "Horo/Navigation/NavigationSettingsErrors.h",
        "Horo/Application/NavigationProjectSettingsStore.h",
        "Horo/Editor/NavigationDefinitionDocument.h",
        "Horo/Editor/NavigationSettingsWorkflow.h",
        "Horo/Editor/NavigationProjectSettingsSession.h",
    ):
        assert ownership.count(header) == 1
