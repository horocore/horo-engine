"""Headless PCG document, scene output and bake build/discovery contracts."""
from __future__ import annotations

import re

from test_ci_workflow_grouping import ROOT, SUITES, cmake_conditions_at, preset, targets


def test_windows_pcg_document_qualification_has_build_execution_and_consumer_closure() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    ownership = (ROOT / "cmake/HoroPublicHeaderOwnership.cmake").read_text(encoding="utf-8")
    for target in ("HoroPCGDocumentTests", "HoroPCGDocumentPublicHeaderConsumer"):
        assert target in targets("HORO_CI_WINDOWS_TARGETS")
        assert f"add_executable({target}" in tests_cmake
    assert "horo_register_catch_test(HoroPCGDocumentTests LABELS" in tests_cmake
    assert "add_test(NAME HoroPCGDocumentPublicHeaderConsumer" in tests_cmake
    assert "unit/editor/PCGGraphDocumentTests.cpp" in tests_cmake
    assert "unit/editor/PCGGraphDocumentServiceTests.cpp" in tests_cmake
    for header in ("PCGGraphDocument.h", "PCGGraphDocumentService.h", "DocumentRecoveryService.h", "SemanticDocumentHistory.h"):
        assert f"Horo/Editor/{header}" in ownership


def test_windows_pcg_qualification_has_build_and_execution_closure() -> None:
    tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    for target in ("HoroPCGTests", "HoroPCGTerrainAdapterTests"):
        assert target in targets("HORO_CI_WINDOWS_TARGETS")
        assert f"add_executable({target}" in tests_cmake
        assert f"horo_register_catch_test({target} LABELS" in tests_cmake
    assert tests_cmake.count("unit/runtime/pcg/PCGCpuEvaluatorTests.cpp") == 1
    assert 'if(target IN_LIST HORO_CI_WINDOWS_TARGETS)\n        list(APPEND ARG_LABELS ci-windows)' in SUITES
    consumers = ("HoroPCGPublicHeaderConsumer", "HoroPCGTerrainAdapterPublicHeaderConsumer")
    ownership = (ROOT / "cmake/HoroPublicHeaderOwnership.cmake").read_text(encoding="utf-8")
    boundaries = (ROOT / "cmake/HoroTargetBoundaries.cmake").read_text(encoding="utf-8")
    assert 'add_library("${consumer_target}" OBJECT ${generated_sources})' in boundaries
    direct_tests = re.search(r"set_property\(TEST\s+(.*?)\s+APPEND PROPERTY LABELS ci-windows\)", SUITES, re.S)
    assert direct_tests
    for consumer in consumers:
        assert consumer in targets("HORO_CI_WINDOWS_TARGETS")
        assert consumer not in direct_tests.group(1).split()
        owner = consumer.removesuffix("PublicHeaderConsumer")
        assert f"horo_configure_target_header_boundary({owner} PUBLIC_HEADERS" in ownership
    assert preset("buildPresets", "ci-windows-debug")["targets"] == ["HoroCiWindowsChecks"]
    assert preset("testPresets", "ci-windows-debug")["filter"]["include"]["label"] == "^ci-windows$"


def test_scene_bake_has_canonical_owner_and_platform_consumer_closure() -> None:
    tests = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    registry = (ROOT / "cmake/HoroPublicHeaderOwnership.cmake").read_text(encoding="utf-8")
    source = (ROOT / "src/CMakeLists.txt").read_text(encoding="utf-8")
    required = {"HoroPCGSceneBakeTests", "HoroPCGSceneBakeContractConsumer", "HoroPCGSceneBakePublicHeaderConsumer", "HoroEditorModelPublicHeaderConsumer"}
    assert required <= targets("HORO_CI_WINDOWS_TARGETS")
    for target in ("HoroPCGSceneBakeTests", "HoroPCGSceneBakeContractConsumer"):
        declaration = f"add_executable({target} "
        assert declaration in tests
        assert not cmake_conditions_at(tests, tests.index(declaration)), "Scene bake is available headlessly without a GUI/backend option"
    assert 'horo_register_catch_test(HoroPCGSceneBakeTests LABELS "unit;pcg;scene;headless;ci-windows;sonar-coverage")' in tests
    assert "add_test(NAME HoroPCGSceneBakeContractConsumer COMMAND HoroPCGSceneBakeContractConsumer)" in tests
    assert 'set_tests_properties(HoroPCGSceneBakeContractConsumer PROPERTIES LABELS "unit;pcg;scene;headless;public_headers;ci-windows")' in tests
    assert "Horo/Editor/SceneDocumentTransaction.h" in registry
    assert "Horo/PCGScene/SceneBake.h" in registry
    assert "editor/document/SceneDocumentTransaction.cpp" in source
    assert "target_link_libraries(HoroPCGSceneBake PUBLIC HoroEngine::PCGSceneAdapter HoroEngine::EditorServices HoroEngine::EditorModel)" in source
    adapter = (ROOT / "src/editor/pcg/PCGSceneBake.cpp").read_text(encoding="utf-8")
    assert '#include "editor/document/' not in adapter
