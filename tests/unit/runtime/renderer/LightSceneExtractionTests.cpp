#include "Horo/Runtime/Render/LightSceneExtraction.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Runtime;

    /** @brief Build the actual runtime scene authority without editor dependencies. */
    std::unique_ptr<RuntimeScene> Scene(std::vector<RuntimeEntityDefinition> entities) {
        SceneDefinitionBuilder builder{{1}, {1}};
        for (auto &entity : entities)
            builder.Add(std::move(entity));
        auto definition = std::move(builder).Build();
        REQUIRE(definition.HasValue());
        auto scene = RuntimeScene::Create(definition.Value(), {7});
        REQUIRE(scene.HasValue());
        return std::move(scene).Value();
    }

    /** @brief Produce a scene entity with an enabled typed authored light. */
    RuntimeEntityDefinition LightObject(std::uint64_t object, LightKind kind) {
        RuntimeEntityDefinition entity;
        entity.object = {object};
        entity.components.light = LightComponent{.kind = kind};
        return entity;
    }
}  // namespace

TEST_CASE("Runtime extraction supports all light kinds and preserves authored cone units", "[runtime][renderer][light-extraction]") {
    auto scene = Scene({LightObject(1, LightKind::Directional), LightObject(2, LightKind::Point), LightObject(3, LightKind::Spot)});
    std::array<IdentifiedRenderLight, 3> scratch;
    const auto result = ExtractSceneLights(scene->View(), 3, scratch);
    REQUIRE(result.HasValue());
    CHECK(result.Value().scene == SceneRuntimeId{7});
    CHECK(result.Value().structuralRevision == scene->View().StructuralRevision());
    CHECK(result.Value().count == 3);
    CHECK(scratch[0].identity < scratch[1].identity);
    CHECK(scratch[1].identity < scratch[2].identity);
    CHECK(scratch[0].light.kind == RenderLightKind::Directional);
    CHECK(scratch[1].light.kind == RenderLightKind::Point);
    CHECK(scratch[2].light.kind == RenderLightKind::Spot);
    CHECK(Math::NearlyEqual(scratch[2].light.innerConeCosine, std::cos(LightComponent{}.innerConeRadians)));
    CHECK(Math::NearlyEqual(scratch[2].light.outerConeCosine, std::cos(LightComponent{}.outerConeRadians)));
}

TEST_CASE("Runtime light pose respects hierarchy without scaling range or direction", "[runtime][renderer][light-extraction]") {
    RuntimeEntityDefinition parent;
    parent.object = {1};
    parent.localTransform.translation = {10, 0, 0};
    parent.localTransform.scale = {2, 3, 4};
    parent.localTransform.rotation = Math::Quaternion::FromAxisAngle({0, 1, 0}, Math::Pi / 2);
    auto child = LightObject(2, LightKind::Spot);
    child.parent = SceneObjectId{1};
    child.localTransform.translation = {0, 0, -1};
    auto scene = Scene({parent, child});
    std::array<IdentifiedRenderLight, 1> scratch;
    REQUIRE(ExtractSceneLights(scene->View(), 1, scratch).HasValue());
    CHECK(Math::NearlyEqual(scratch[0].light.position.x, 6));
    CHECK(Math::NearlyEqual(scratch[0].light.direction.x, -1));
    CHECK(Math::NearlyEqual(Math::Length(scratch[0].light.direction), 1));
    CHECK(scratch[0].light.range == LightComponent{}.range);
}

TEST_CASE("Runtime extraction skips disabled lights and rejects bounded overflow", "[runtime][renderer][light-extraction]") {
    auto disabled = LightObject(1, LightKind::Directional);
    disabled.components.light->enabled = false;
    auto scene = Scene({disabled, LightObject(2, LightKind::Point), LightObject(3, LightKind::Spot)});
    std::array<IdentifiedRenderLight, 2> scratch;
    const auto result = ExtractSceneLights(scene->View(), 2, scratch);
    REQUIRE(result.HasValue());
    CHECK(result.Value().count == 2);
    const auto overflow = ExtractSceneLights(scene->View(), 1, scratch);
    REQUIRE(overflow.HasError());
    CHECK(overflow.ErrorValue().code.Value() == LightCullingErrors::Capacity.code.Value());
    CHECK(ExtractSceneLights(scene->View(), 2, std::span{scratch}.first(1)).HasError());
}

TEST_CASE("Runtime extraction does not publish cancelled stale or invalid tables", "[runtime][renderer][light-extraction]") {
    auto scene = Scene({LightObject(1, LightKind::Point)});
    std::array<IdentifiedRenderLight, 1> scratch;
    CancellationSource source;
    source.RequestCancellation();
    const auto cancelled = ExtractSceneLights(scene->View(), 1, scratch, source.Token());
    REQUIRE(cancelled.HasError());
    CHECK(cancelled.ErrorValue().code.Value() == LightCullingErrors::Cancelled.code.Value());
    CHECK(ExtractSceneLights({}, 1, scratch).HasError());
    CHECK(ExtractSceneLights(scene->View(), 0, scratch).HasError());
    const auto stale = scene->View();
    SceneCommandBuffer commands;
    commands.SetLocalTransform(*stale.Find({1}), Math::Transform{});
    REQUIRE(scene->Commit(commands).HasValue());
    CHECK(ExtractSceneLights(stale, 1, scratch).HasError());
}
