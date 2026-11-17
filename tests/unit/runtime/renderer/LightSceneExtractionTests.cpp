#include "Horo/Runtime/Render/LightSceneExtraction.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Runtime;
    constexpr SceneColorSourcePolicy Legacy{SceneColorEncoding::LegacyLinearRec709D65, 1};
    constexpr SceneColorSourcePolicy Canonical{SceneColorEncoding::LinearAcesCg, 1};

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
    const auto result = ExtractSceneLights(scene->View(), 3, scratch, Legacy);
    REQUIRE(result.HasValue());
    CHECK(result.Value().scene == SceneRuntimeId{7});
    CHECK(result.Value().structuralRevision == scene->View().StructuralRevision());
    CHECK(result.Value().count == 3);
    CHECK(result.Value().sourceColorPolicy == Legacy);
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
    REQUIRE(ExtractSceneLights(scene->View(), 1, scratch, Legacy).HasValue());
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
    const auto result = ExtractSceneLights(scene->View(), 2, scratch, Legacy);
    REQUIRE(result.HasValue());
    CHECK(result.Value().count == 2);
    const auto overflow = ExtractSceneLights(scene->View(), 1, scratch, Legacy);
    REQUIRE(overflow.HasError());
    CHECK(overflow.ErrorValue().code.Value() == LightCullingErrors::Capacity.code.Value());
    CHECK(ExtractSceneLights(scene->View(), 2, std::span{scratch}.first(1), Legacy).HasError());
}

TEST_CASE("Runtime extraction does not publish cancelled stale or invalid tables", "[runtime][renderer][light-extraction]") {
    auto scene = Scene({LightObject(1, LightKind::Point)});
    std::array<IdentifiedRenderLight, 1> scratch;
    CancellationSource source;
    source.RequestCancellation();
    const auto cancelled = ExtractSceneLights(scene->View(), 1, scratch, Legacy, source.Token());
    REQUIRE(cancelled.HasError());
    CHECK(cancelled.ErrorValue().code.Value() == LightCullingErrors::Cancelled.code.Value());
    CHECK(ExtractSceneLights({}, 1, scratch, Legacy).HasError());
    CHECK(ExtractSceneLights(scene->View(), 0, scratch, Legacy).HasError());
    const auto stale = scene->View();
    SceneCommandBuffer commands;
    commands.SetLocalTransform(*stale.Find({1}), Math::Transform{});
    REQUIRE(scene->Commit(commands).HasValue());
    CHECK(ExtractSceneLights(stale, 1, scratch, Legacy).HasError());
}

TEST_CASE("Runtime light extraction converts declared legacy RGB before canonical lighting", "[runtime][renderer][light-extraction]") {
    auto entity = LightObject(1, LightKind::Point);
    entity.components.light->color = {1, 0, 0};
    entity.components.light->intensity = 2;
    auto scene = Scene({entity});
    std::array<IdentifiedRenderLight, 1> scratch;
    const auto result = ExtractSceneLights(scene->View(), 1, scratch, Legacy);
    REQUIRE(result.HasValue());
    CHECK(result.Value().sourceColorPolicy == Legacy);
    CHECK(scratch[0].light.color.x == 0.6130974292755127F);
    CHECK(scratch[0].light.color.y == 0.07019372284412384F);
    CHECK(scratch[0].light.color.z == 0.0206155925989151F);
    CHECK(scratch[0].light.intensity == 2);
}

TEST_CASE("Runtime extraction preserves declared AP1 HDR and reports its source policy", "[runtime][renderer][light-extraction]") {
    auto entity = LightObject(1, LightKind::Directional);
    entity.components.light->color = {0.25F, 0.18F, 4};
    auto scene = Scene({entity});
    std::array<IdentifiedRenderLight, 1> scratch;
    const auto result = ExtractSceneLights(scene->View(), 1, scratch, Canonical);
    REQUIRE(result.HasValue());
    CHECK(result.Value().sourceColorPolicy == Canonical);
    CHECK(scratch[0].light.IsValid());
    CHECK(scratch[0].light.color.x == 0.25F);
    CHECK(scratch[0].light.color.y == 0.18F);
    CHECK(scratch[0].light.color.z == 4);
}

TEST_CASE("Canonical light value boundary preserves signed AP1 without changing authored Scene admission",
          "[runtime][renderer][light-extraction]") {
    LightComponent light;
    light.color = {-0.25F, 0.18F, 4};
    const auto result = BuildRenderLightValue(light, {}, {0, 0, -1}, Canonical);
    REQUIRE(result.HasValue());
    CHECK(result.Value().IsValid());
    CHECK(result.Value().color.x == -0.25F);
    CHECK(result.Value().color.y == 0.18F);
    CHECK(result.Value().color.z == 4);
}

TEST_CASE("Runtime extraction rejects unspecified color before publishing even an empty table", "[runtime][renderer][light-extraction]") {
    auto scene = Scene({});
    std::array<IdentifiedRenderLight, 1> scratch;
    const auto result = ExtractSceneLights(scene->View(), 1, scratch, {});
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == SceneColorErrors::InvalidPolicy.code.Value());
}

TEST_CASE("Canonical light conversion preserves existing authored light validation", "[runtime][renderer][light-extraction]") {
    LightComponent light;
    SECTION("disabled") {
        light.enabled = false;
    }
    SECTION("negative intensity") {
        light.intensity = -1;
    }
    SECTION("invalid range") {
        light.range = -1;
    }
    SECTION("reversed cones") {
        light.innerConeRadians = light.outerConeRadians + 0.1F;
    }
    SECTION("unknown kind") {
        light.kind = static_cast<LightKind>(255);
    }
    CHECK(BuildRenderLightValue(light, {}, {0, 0, -1}, Canonical).HasError());
}
