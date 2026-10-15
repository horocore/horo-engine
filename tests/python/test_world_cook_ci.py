"""Owning world artifact codec tests must be built and discovered on Windows."""
from test_ci_workflow_grouping import ROOT, targets


def test_world_artifact_codec_has_real_owner_and_consumer_closure() -> None:
    source = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    assert {"HoroWorldArtifactCodecTests", "HoroWorldArtifactCodecConsumer"} <= targets("HORO_CI_WINDOWS_TARGETS")
    assert "unit/runtime/world_streaming/WorldArtifactCodecTests.cpp" in source
    assert 'horo_register_catch_test(HoroWorldArtifactCodecTests LABELS "unit;world_streaming;headless;ci-windows")' in source
    assert 'add_test(NAME HoroWorldArtifactCodecConsumer COMMAND HoroWorldArtifactCodecConsumer)' in source
    assert "support/WorldArtifactCodecPublicConsumer.cpp" in source


def test_scene_semantic_codec_retains_owner_and_actual_schema_regressions() -> None:
    assert {"HoroRuntimeSceneCellPayloadTests", "HoroSceneCellPayloadPublicHeaderConsumer"} <= targets("HORO_CI_WINDOWS_TARGETS")
    source = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    assert "unit/runtime/scene/SceneCellPayloadCodecTests.cpp" in source
    assert "unit/runtime/scene/SceneCellContentHasherTests.cpp" in source
    ownership = (ROOT / "cmake/HoroPublicHeaderOwnership.cmake").read_text(encoding="utf-8")
    assert "Horo/Runtime/Scene/SceneCellPayloadCodec.h" in ownership


def test_world_durable_composition_has_real_owner_runtime_and_public_consumer() -> None:
    assert {"HoroWorldCookIntegrationTests", "HoroWorldCookIntegrationConsumer",
            "HoroWorldCookIntegrationPublicHeaderConsumer", "HoroWorldStreamingPublicHeaderConsumer"} <= targets("HORO_CI_WINDOWS_TARGETS")
    source = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    assert "unit/world_cook/WorldCookPublicationTests.cpp" in source
    assert "support/WorldCookPublicConsumer.cpp" in source
    assert 'horo_register_catch_test(HoroWorldCookIntegrationTests LABELS "unit;world_streaming;headless;ci-windows")' in source
    assert 'add_test(NAME HoroWorldCookIntegrationConsumer COMMAND HoroWorldCookIntegrationConsumer)' in source
    ownership = (ROOT / "cmake/HoroPublicHeaderOwnership.cmake").read_text(encoding="utf-8")
    assert "Horo/WorldCook/WorldCookPublication.h" in ownership
