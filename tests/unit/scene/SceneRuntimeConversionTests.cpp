#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Scene/SceneRuntimeConversion.h"
#include "SceneSourceInternal.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <limits>

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

        PrefabSourceResolverSnapshot Resolver(const std::string_view rootName = "Root", std::vector<RawComponentPayload> components = {},
                                              std::vector<RawComponentPayload> childComponents = {}) {
            REQUIRE(registry
                        .Publish({{asset, Assets::AssetTypeId::Parse("core.prefab").Value(),
                                   ProjectPath::Parse("assets/hierarchy.prefab").Value(),
                                   ProjectPath::Parse("assets/hierarchy.prefab.horo").Value()}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
            auto document =
                PrefabDocument::Create({.projectVersion = version,
                                        .assetId = asset,
                                        .objects = {{.localId = {0}, .name = std::string{rootName}, .components = std::move(components)},
                                                    {.localId = {3},
                                                     .parentLocalId = LocalObjectId{0},
                                                     .name = "Child",
                                                     .localTransform = {.translation = {0, 4, 0}},
                                                     .components = std::move(childComponents)}}},
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
    const auto previous = ConvertScenePrefabProjectionToRuntime(source, {1}, {2}, retained, {resolver, fixture.limits});
    REQUIRE(previous.HasValue());

    const auto current = fixture.Resolver("Edited");
    REQUIRE(ConvertScenePrefabProjectionToRuntime(source, {1}, {2}, retained, {current, fixture.limits}).HasError());
    const std::array changed{fixture.asset};
    InvalidateScenePrefabProjection(retained, current, changed, fixture.limits);
    REQUIRE(retained.instances.front().stale);
    REQUIRE_FALSE(retained.instances.front().IsSynchronized());
    CHECK(retained.instances.front().expanded->Objects().front().object.name == "Root");
    InvalidateScenePrefabProjection(retained, resolver, {}, fixture.limits);
    REQUIRE(ConvertScenePrefabProjectionToRuntime(source, {1}, {2}, retained, {resolver, fixture.limits}).HasError());

    const auto refreshed = BuildScenePrefabProjection(source, current, fixture.limits).Value();
    const auto replacement = ConvertScenePrefabProjectionToRuntime(source, {1}, {2}, refreshed, {current, fixture.limits});
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

TEST_CASE("Prefab Physics local endpoints reject wide Scene IDs instead of aliasing a local object", "[native][prefab-cook][physics]") {
    ConversionFixture fixture;
    const auto endpoint = GENERATE(std::uint64_t{3}, (std::uint64_t{1} << 32U) | 3U, std::numeric_limits<std::uint64_t>::max());
    CAPTURE(endpoint);
    SceneObjectComponentSet typed;
    typed.rigidBody = Runtime::RigidBodyComponent{.id = {1}, .body = {1}};
    typed.colliders.push_back({.id = {2},
                               .collider = {1},
                               .body = {{endpoint}, {1}},
                               .collisionProfile = Physics::CollisionProfileId::Parse("00000000-0000-0000-0000-000000000001").Value(),
                               .materials = {{Physics::PhysicsMaterialSlotId::FromValue(1),
                                              Assets::AssetId::Parse("00000000-0000-0000-0000-000000000002").Value()}}});
    const auto json = Detail::ComponentsJson(typed).ToJson();
    const auto payload = [](const std::string_view type, const std::string &bytes, const std::uint64_t occurrence) {
        RawComponentPayload result{.instance = PrefabComponentInstanceId::Create(occurrence).Value(),
                                   .component = {.typeId = Gameplay::ComponentTypeId::Parse(type).Value(),
                                                 .schemaVersion = 1,
                                                 .encoding = Gameplay::ComponentPayloadEncoding::CanonicalJson}};
        for (const unsigned char value : bytes)
            result.component.payload.push_back(static_cast<std::byte>(value));
        return result;
    };
    const auto resolver = fixture.Resolver("Physics", {},
                                           {payload("game.horo.rigid_body", json.at("rigidBody").dump(), 1),
                                            payload("game.horo.collider", json.at("colliders").at(0).dump(), 2)});
    const std::vector<SceneObjectSnapshot> objects{{.id = {900}, .name = "Containing scene"}};
    const std::vector<ScenePrefabInstance> placements{fixture.Placement(7)};
    const auto converted = ConvertSceneSourceToRuntime({objects, placements}, {1}, {2}, resolver, fixture.limits);
    std::string diagnostic;
    if (converted.HasError())
        for (const Error *error = &converted.ErrorValue(); error; error = error->cause.Get())
            diagnostic.append(error->code.Value()).append(": ").append(error->message).append("\n");
    INFO(diagnostic);
    if (endpoint > std::numeric_limits<std::uint32_t>::max()) {
        REQUIRE(converted.HasError());
        bool rejectedWideIdentity{};
        for (const Error *error = &converted.ErrorValue(); error; error = error->cause.Get())
            rejectedWideIdentity = rejectedWideIdentity || error->code.Value() == PrefabErrors::ReferenceRewriteInvalid.code.Value();
        CHECK(rejectedWideIdentity);
    } else {
        REQUIRE(converted.HasValue());
        const auto &child = converted.Value().Entities()[2];
        REQUIRE(child.components.colliders.size() == 1);
        CHECK(child.components.colliders.front().body.object == child.object);
    }
    CHECK(typed.colliders.front().body.object.value == endpoint);
}
