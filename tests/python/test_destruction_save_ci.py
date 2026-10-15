"""Destruction Save integration owns its platform build and execution closure."""
from test_ci_workflow_grouping import ROOT, SUITES, cmake_conditions_at, targets


def test_destruction_save_and_replication_consumers_have_build_and_execution_closure():
    expected = {
        "HoroDestructionSaveTests", "HoroDestructionSavePublicConsumer",
        "HoroDestructionSaveIntegrationPublicHeaderConsumer", "HoroDestructionApiPublicHeaderConsumer",
        "HoroDestructionReplicationTests", "HoroDestructionReplicationPublicHeaderConsumer",
    }
    assert expected <= targets("HORO_CI_WINDOWS_TARGETS")
    source = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    for target in ("HoroDestructionSaveTests", "HoroDestructionSavePublicConsumer"):
        assert not cmake_conditions_at(source, source.index(f"add_executable({target}"))
    assert 'horo_register_catch_test(HoroDestructionSaveTests LABELS "unit;destruction;save;headless;ci-windows;sonar-coverage")' in source
    assert 'add_test(NAME HoroDestructionSavePublicConsumer COMMAND HoroDestructionSavePublicConsumer)' in source
    assert 'horo_register_catch_test(HoroDestructionReplicationTests LABELS "unit;destruction;network;headless;ci-windows")' in source
    ownership = (ROOT / "cmake/HoroPublicHeaderOwnership.cmake").read_text(encoding="utf-8")
    assert "Horo/Destruction/DestructionCanonicalState.h" in ownership
    assert "Horo/Destruction/DestructionSaveCapture.h" in ownership
    production = (ROOT / "src/CMakeLists.txt").read_text(encoding="utf-8")
    assert "target_link_libraries(HoroDestructionSaveIntegration PUBLIC HoroEngine::DestructionRuntime HoroEngine::Runtime)" in production
    for path in (ROOT / "src/runtime/destruction/save").glob("*.cpp"):
        assert 'Horo/Network/' not in path.read_text(encoding="utf-8")
    assert "HoroDestructionSaveTests" in SUITES
