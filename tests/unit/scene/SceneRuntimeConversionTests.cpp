#include "Horo/Scene/SceneRuntimeConversion.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Prefab;
    using namespace Horo::SceneSource;

    /** @brief Supplies a complete canonical prefab document and its real serialized source digest. */
    struct ConversionFixture final {
        const Assets::AssetId asset = Assets::AssetId::FromBytes({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
        const PrefabLimitProfile limits = PrefabLimitProfile::Create({}).Value();
        const Application::HoroVersion version = Application::ParseHoroVersion("1.2.3").Value();
        Assets::AssetRegistry registry;

        PrefabSourceResolverSnapshot Resolver(const std::string_view rootName = "Root") {
            REQUIRE(registry
                        .Publish({{asset, Assets::AssetTypeId::Parse("core.prefab").Value(),
                                   ProjectPath::Parse("assets/hierarchy.prefab").Value(),
                                   ProjectPath::Parse("assets/hierarchy.prefab.horo").Value()}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
            auto document = PrefabDocument::Create({.projectVersion = version,
                                                    .assetId = asset,
                                                    .objects = {{.localId = {0}, .name = std::string{rootName}},
                                                                {.localId = {3},
                                                                 .parentLocalId = LocalObjectId{0},
                                                                 .name = "Child",
                                                                 .localTransform = {.translation = {0, 4, 0}}}}},
                                                   limits);
            REQUIRE(document.HasValue());
            auto canonical = document.Value().SerializeCanonical();
            REQUIRE(canonical.HasValue());
            const auto digest = ComputeSha256(std::as_bytes(std::span{canonical.Value()}));
            auto resolver =
                BuildPrefabSourceResolverSnapshot(registry.Snapshot(),
                                                  {{std::move(document).Value(), {.projectVersion = version, .contentDigest = digest}}},
                                                  limits);
            REQUIRE(resolver.HasValue());
            return std::move(resolver).Value();
        }

        ScenePrefabInstance Placement(const std::uint64_t instance) const {
            return {.instanceId = PrefabInstanceId::Create(instance).Value(),
                    .sourcePrefab = PrefabAssetReference::Create(asset).Value(),
                    .parent = SceneObjectId{900},
                    .rootTransform = {.translation = {10, 0, 0}}};
        }
    };
}  // namespace

TEST_CASE("Headless retained conversion fences source changes until a fresh projection is resolved", "[native][prefab-cook]") {
    ConversionFixture fixture;
    const auto resolver = fixture.Resolver();
    const std::vector<SceneObjectSnapshot> objects{{.id = {900}, .name = "Containing scene"}};
    const std::vector<ScenePrefabInstance> placements{fixture.Placement(7)};
    const SceneSourceView source{objects, placements};
    auto retained = BuildScenePrefabProjection(source, resolver, fixture.limits).Value();
    const auto previous = ConvertScenePrefabProjectionToRuntime(source, {1}, {2}, retained, resolver, fixture.limits);
    REQUIRE(previous.HasValue());

    const auto current = fixture.Resolver("Edited");
    REQUIRE(ConvertScenePrefabProjectionToRuntime(source, {1}, {2}, retained, current, fixture.limits).HasError());
    const std::array changed{fixture.asset};
    InvalidateScenePrefabProjection(retained, current, changed, fixture.limits);
    REQUIRE(retained.instances.front().stale);
    REQUIRE_FALSE(retained.instances.front().IsSynchronized());
    CHECK(retained.instances.front().expanded->Objects().front().object.name == "Root");
    InvalidateScenePrefabProjection(retained, resolver, {}, fixture.limits);
    REQUIRE(ConvertScenePrefabProjectionToRuntime(source, {1}, {2}, retained, resolver, fixture.limits).HasError());

    const auto refreshed = BuildScenePrefabProjection(source, current, fixture.limits).Value();
    const auto replacement = ConvertScenePrefabProjectionToRuntime(source, {1}, {2}, refreshed, current, fixture.limits);
    REQUIRE(replacement.HasValue());
    CHECK(replacement.Value().Entities()[1].object == previous.Value().Entities()[1].object);
    CHECK(previous.Value().Entities().size() == 3);
}

TEST_CASE("Headless scene conversion expands repeated placements through the shared resolver", "[native][prefab-cook]") {
    ConversionFixture fixture;
    const auto resolver = fixture.Resolver();
    const std::vector<SceneObjectSnapshot> objects{{.id = {900}, .name = "Containing scene"}};
    const std::vector<ScenePrefabInstance> placements{fixture.Placement(7), fixture.Placement(8)};
    const SceneSourceView source{objects, placements};
    const auto converted = ConvertSceneSourceToRuntime(source, {1}, {2}, resolver, fixture.limits);
    REQUIRE(converted.HasValue());
    REQUIRE(converted.Value().Entities().size() == 5);
    const auto entities = converted.Value().Entities();
    REQUIRE(entities[1].parent == Runtime::SceneObjectId{900});
    REQUIRE(entities[3].parent == Runtime::SceneObjectId{900});
    REQUIRE(entities[1].object != entities[3].object);
    REQUIRE(entities[2].parent == entities[1].object);
    REQUIRE(entities[4].parent == entities[3].object);
    REQUIRE(entities[1].localTransform.translation == Math::Vec3{10, 0, 0});
    REQUIRE(entities[2].localTransform.translation == Math::Vec3{0, 4, 0});
    REQUIRE(placements.front().sourcePrefab.Asset() == fixture.asset);
}

TEST_CASE("Headless scene conversion rejects incomplete expansion without publishing partial content", "[native][prefab-cook]") {
    ConversionFixture fixture;
    const auto resolver = fixture.Resolver();
    const std::vector<SceneObjectSnapshot> objects{{.id = {900}}};
    auto placement = fixture.Placement(7);
    SECTION("external parent missing") {
        placement.parent = SceneObjectId{999};
    }
    SECTION("duplicate instance identity") {}
    const std::vector<ScenePrefabInstance> placements{placement, placement};
    const auto converted = ConvertSceneSourceToRuntime({objects, placements}, {1}, {2}, resolver, fixture.limits);
    REQUIRE(converted.HasError());
    REQUIRE(objects.size() == 1);
    REQUIRE(placements.size() == 2);
    REQUIRE(ConvertSceneSourceToRuntime({objects, placements}, {1}, {2}).HasError());
}
