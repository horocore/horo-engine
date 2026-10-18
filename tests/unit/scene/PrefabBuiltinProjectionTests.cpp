#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "SceneSourceInternal.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::SceneSource;
    using Kind = Runtime::BuiltinSceneComponentKind;

    /** @brief Creates the actual portable envelope consumed by both source and cooked projection. */
    Prefab::RawComponentPayload Payload(const std::string_view type, const std::string_view json, const std::uint64_t occurrence = 1) {
        Prefab::RawComponentPayload result{.instance = Prefab::PrefabComponentInstanceId::Create(occurrence).Value(),
                                           .component = {.typeId = Gameplay::ComponentTypeId::Parse(type).Value(),
                                                         .schemaVersion = 1,
                                                         .encoding = Gameplay::ComponentPayloadEncoding::CanonicalJson}};
        for (const char byte : json)
            result.component.payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
        return result;
    }

    /** @brief Selects the stable schema ID without introducing a second type-ID registry in tests. */
    std::string_view Type(const Kind kind) {
        for (const auto &schema : Runtime::BuiltinSceneComponentSchemas())
            if (schema.kind == kind)
                return schema.typeId;
        FAIL("Missing built-in schema");
        return {};
    }

    /** @brief Checks preserved typed values and duplicate rejection using the existing canonical encoder. */
    template <typename T>
    void RequireOptional(const Kind kind, const std::string_view field, std::optional<T> SceneObjectComponentSet::*sourceField,
                         std::optional<T> Runtime::RuntimeComponentSet::*runtimeField, const T &expected) {
        SceneObjectComponentSet authored;
        authored.*sourceField = expected;
        const auto encoded = Detail::ComponentsJson(authored).ToJson();
        const auto raw = Payload(Type(kind), encoded.at(std::string(field)).dump());
        Runtime::RuntimeComponentSet projected;
        std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
        auto decoded = ProjectPrefabBuiltinComponent(raw, projected, primitive);
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value());
        REQUIRE((projected.*runtimeField).has_value());
        CHECK(*(projected.*runtimeField) == expected);
        CHECK_FALSE(primitive);
        CHECK(Runtime::BuiltinSceneComponentCount(projected, kind) == 1);
        const auto retained = projected;
        CHECK(ProjectPrefabBuiltinComponent(raw, projected, primitive).HasError());
        CHECK(projected == retained);
    }
}  // namespace

TEST_CASE("Canonical prefab projection preserves disabled camera light and audio values", "[prefab][codec][components]") {
    RequireOptional(Kind::Camera, "camera", &SceneObjectComponentSet::camera, &Runtime::RuntimeComponentSet::camera,
                    Runtime::CameraComponent{.enabled = false});
    RequireOptional(Kind::Light, "light", &SceneObjectComponentSet::light, &Runtime::RuntimeComponentSet::light,
                    Runtime::LightComponent{.enabled = false});
    RequireOptional(Kind::AudioSource, "audioSource", &SceneObjectComponentSet::audioSource, &Runtime::RuntimeComponentSet::audioSource,
                    Runtime::AudioSourceComponent{.enabled = false});
}

TEST_CASE("Canonical prefab AI projection preserves identity policy and unavailable descriptor references", "[prefab][codec][ai]") {
    const AI::AiAgentComponent agent{.agent = AI::AgentId::Create(17).Value(),
                                     .startupPolicy = AI::AiStartupPolicy::Manual,
                                     .enabled = false};
    RequireOptional(Kind::AiAgent, "aiAgent", &SceneObjectComponentSet::aiAgent, &Runtime::RuntimeComponentSet::aiAgent, agent);
    const AI::AiControllerComponent controller{.controller = AI::ControllerTypeId::Create(23).Value(),
                                               .decisionAsset = AI::DecisionGraphAssetId::Create(29).Value(),
                                               .blackboardSchema = AI::BlackboardSchemaId::Create(31).Value(),
                                               .startupPolicy = AI::AiStartupPolicy::OnEnable,
                                               .enabled = false};
    RequireOptional(Kind::AiController, "aiController", &SceneObjectComponentSet::aiController, &Runtime::RuntimeComponentSet::aiController,
                    controller);
}

TEST_CASE("Canonical prefab Navigation surfaces and regions preserve complete disabled authored values", "[prefab][codec][navigation]") {
    const auto surface = Navigation::SurfaceId::Create(3).Value();
    const auto profile = Navigation::NavigationAgentProfileId::Create(5).Value();
    RequireOptional(Kind::NavigationSurface, "navigationSurface", &SceneObjectComponentSet::navigationSurface,
                    &Runtime::RuntimeComponentSet::navigationSurface,
                    Runtime::NavigationSurfaceComponent{.id = surface,
                                                        .definition =
                                                            Assets::AssetId::Parse("00000000-0000-0000-0000-000000000007").Value(),
                                                        .generation = 7,
                                                        .bakeScope = Runtime::NavigationBakeScope::LocalBounds,
                                                        .localBounds = Runtime::NavigationLocalBounds{{1, 2, 3}, {4, 5, 6}},
                                                        .profiles = {profile},
                                                        .enabled = false});
    RequireOptional(Kind::NavigationRegion, "navigationRegion", &SceneObjectComponentSet::navigationRegion,
                    &Runtime::RuntimeComponentSet::navigationRegion,
                    Runtime::NavigationRegionComponent{.id = Navigation::NavigationRegionId::Create(11).Value(),
                                                       .surface = surface,
                                                       .generation = 13,
                                                       .localBounds = {{1, 2, 3}, {4, 5, 6}},
                                                       .sourceSelection = Runtime::NavigationRegionSourceSelection::StaticCollisionInBounds,
                                                       .mode = Runtime::NavigationRegionMode::Exclude,
                                                       .enabled = false});
}

TEST_CASE("Canonical prefab Navigation modifiers links and agents preserve typed cross references", "[prefab][codec][navigation]") {
    const auto surface = Navigation::SurfaceId::Create(3).Value();
    RequireOptional(Kind::NavigationModifier, "navigationModifier", &SceneObjectComponentSet::navigationModifier,
                    &Runtime::RuntimeComponentSet::navigationModifier,
                    Runtime::NavigationModifierComponent{.id = Navigation::NavigationModifierId::Create(17).Value(),
                                                         .surface = surface,
                                                         .volume = Runtime::NavigationCylinderVolume{{1, 2, 3}, 2, 4},
                                                         .operation = Runtime::NavigationModifierOperation::OverrideAreaAndCost,
                                                         .area = Navigation::NavigationAreaId::Create(19).Value(),
                                                         .traversalCost = 2.5F,
                                                         .enabled = false});
    RequireOptional(Kind::NavigationLink, "navigationLink", &SceneObjectComponentSet::navigationLink,
                    &Runtime::RuntimeComponentSet::navigationLink,
                    Runtime::NavigationLinkComponent{.id = Navigation::NavigationLinkId::Create(23).Value(),
                                                     .start = {surface, {1, 2, 3}, 0.5F},
                                                     .end = {surface, {4, 5, 6}, 0.75F},
                                                     .direction = Runtime::NavigationLinkDirection::Bidirectional,
                                                     .profiles = {Navigation::NavigationAgentProfileId::Create(5).Value()},
                                                     .traversalCost = 3,
                                                     .enabled = false});
    RequireOptional(Kind::NavigationAgent, "navigationAgent", &SceneObjectComponentSet::navigationAgent,
                    &Runtime::RuntimeComponentSet::navigationAgent,
                    Runtime::NavigationAgentComponent{.profile = Navigation::NavigationAgentProfileId::Create(5).Value(),
                                                      .filter = Navigation::NavigationFilterId::Create(29).Value(),
                                                      .radiusOverride = 0.75F,
                                                      .enabled = false});
}

TEST_CASE("Canonical prefab Character and rigid body projection preserve disabled authored configuration", "[prefab][codec][physics]") {
    RequireOptional(Kind::CharacterController, "characterController", &SceneObjectComponentSet::characterController,
                    &Runtime::RuntimeComponentSet::characterController, Runtime::CharacterControllerComponent{.id = {7}});
    const Runtime::RigidBodyComponent body{.id = {11}, .body = {13}, .enabled = false};
    RequireOptional(Kind::RigidBody, "rigidBody", &SceneObjectComponentSet::rigidBody, &Runtime::RuntimeComponentSet::rigidBody, body);
}

TEST_CASE("Canonical prefab projection retains primitive geometry and rejects a duplicate before mutation", "[prefab][codec][mesh]") {
    const Runtime::PrimitiveMeshDescriptor expected;
    const auto raw = Payload(Type(Kind::PrimitiveMesh), Detail::PrimitiveJson(expected).ToJson().dump());
    Runtime::RuntimeComponentSet components;
    std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
    auto projected = ProjectPrefabBuiltinComponent(raw, components, primitive);
    REQUIRE(projected.HasValue());
    REQUIRE(projected.Value());
    REQUIRE(primitive);
    CHECK(*primitive == expected);
    CHECK(Runtime::BuiltinSceneComponentCount(components, true) == 1);
    CHECK(ProjectPrefabBuiltinComponent(raw, components, primitive).HasError());
    CHECK(*primitive == expected);
}

TEST_CASE("Canonical prefab projection rejects malformed duplicate and unsupported values without changing the candidate",
          "[prefab][codec][failure]") {
    Runtime::RuntimeComponentSet components{.camera = Runtime::CameraComponent{}};
    const auto original = components;
    std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
    std::string type{Type(Kind::Light)};
    std::string json;
    SECTION("duplicate JSON keys") {
        json = R"({"kind":"point","kind":"spot"})";
    }
    SECTION("truncated JSON") {
        json = R"({"kind":)";
    }
    SECTION("missing required fields") {
        json = R"({"kind":"point"})";
    }
    SECTION("unsupported audio listener codec") {
        type = Type(Kind::AudioListener);
        json = "{}";
    }
    SECTION("unsupported runtime canvas codec") {
        type = Type(Kind::UiCanvas);
        json = "{}";
    }
    const auto raw = Payload(type, json);
    CHECK(ProjectPrefabBuiltinComponent(raw, components, primitive).HasError());
    CHECK(components == original);
    CHECK_FALSE(primitive);
}

TEST_CASE("Unknown project payload remains available to the explicit custom component authority", "[prefab][codec][custom]") {
    const auto raw = Payload("game.project.health", R"({"health":25})");
    Runtime::RuntimeComponentSet components;
    std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
    auto projected = ProjectPrefabBuiltinComponent(raw, components, primitive);
    REQUIRE(projected.HasValue());
    CHECK_FALSE(projected.Value());
    CHECK(components == Runtime::RuntimeComponentSet{});
    CHECK_FALSE(primitive);
}
