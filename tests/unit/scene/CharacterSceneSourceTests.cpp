#include "Horo/Scene/SceneSource.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::SceneSource {
    namespace {
        template <class Id> Id Binding(std::uint8_t suffix) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = suffix;
            return Id::FromBytes(bytes);
        }

        Runtime::CharacterControllerComponent ConfiguredCharacter() {
            Runtime::CharacterControllerComponent value{.id = {7}, .enabled = true};
            value.geometry.crouchedCapsule = Runtime::PhysicsCapsuleCollider{0.5F, 0.2F};
            value.geometry.up = {0, 0, 1};
            value.geometry.gravity = {0, 0, -5};
            value.movement.steepSlopePolicy = Character::CharacterSteepSlopePolicy::Slide;
            value.movement.maximumContacts = Character::MaximumCharacterContacts;
            value.movement.dynamicPlatformSupport = true;
            value.movement.inheritPlatformHeading = true;
            value.movement.platformLeaveVelocity = Character::CharacterPlatformLeaveVelocityPolicy::Point;
            value.collision.profile = Binding<Physics::CollisionProfileId>(1);
            value.collision.queryChannel = Binding<Physics::PhysicsQueryChannelId>(2);
            value.collision.defaultMaterial =
                Runtime::PhysicsColliderMaterialBinding{Physics::PhysicsMaterialSlotId::FromValue(3), Binding<Assets::AssetId>(4)};
            value.collision.requiredLayer = Binding<Physics::CollisionLayerId>(5);
            value.collision.requiredProfile = Binding<Physics::CollisionProfileId>(6);
            return value;
        }

        std::string EncodedCharacter(const Runtime::CharacterControllerComponent &character) {
            const std::array objects{SceneObjectSnapshot{.id = {1}, .name = "Character", .components = {.characterController = character}}};
            return EncodeSceneSource({objects, {}});
        }

        void ReplaceOnce(std::string &value, std::string_view from, std::string_view to) {
            const auto offset = value.find(from);
            REQUIRE(offset != std::string::npos);
            value.replace(offset, from.size(), to);
        }
    }  // namespace

    TEST_CASE("Character source preserves every durable policy and binding without runtime identities",
              "[scene][character][serialization]") {
        const auto character = ConfiguredCharacter();
        REQUIRE(Runtime::ValidateCharacterControllerComponent(character).HasValue());
        const auto encoded = EncodedCharacter(character);
        const auto decoded = DecodeSceneSource(encoded);
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().objects.size() == 1);
        CHECK(decoded.Value().objects[0].components.characterController == character);
        CHECK(EncodeSceneSource({decoded.Value().objects, decoded.Value().prefabInstances}) == encoded);
        CHECK(encoded.find("\"schemaVersion\": 2") != std::string::npos);
        CHECK(encoded.find("generation") == std::string::npos);
        CHECK(encoded.find("worldIdentity") == std::string::npos);
        auto legacyVersion = encoded;
        // The document schema comes last in canonical JSON; the component retains its own schema 1.
        const auto documentVersion = legacyVersion.rfind("\"schemaVersion\": 2");
        REQUIRE(documentVersion != std::string::npos);
        legacyVersion.replace(documentVersion, std::string_view{"\"schemaVersion\": 2"}.size(), "\"schemaVersion\": 1");
        CHECK(DecodeSceneSource(legacyVersion).HasError());
    }

    TEST_CASE("Disabled unconfigured Character remains present while legacy absence remains absent", "[scene][character][serialization]") {
        const Runtime::CharacterControllerComponent disabled{.id = {4}};
        REQUIRE(Runtime::ValidateCharacterControllerComponent(disabled).HasValue());
        const auto decoded = DecodeSceneSource(EncodedCharacter(disabled));
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().objects[0].components.characterController == disabled);
        const auto legacy = DecodeSceneSource(R"({"schemaVersion":1,"objects":[]})");
        REQUIRE(legacy.HasValue());
        CHECK(legacy.Value().objects.empty());
        CHECK(DecodeSceneSource(R"({"schemaVersion":2,"objects":[]})").HasValue());
    }

    TEST_CASE("Character source rejects unknown policies incomplete bindings and malformed scalar types",
              "[scene][character][serialization]") {
        for (const auto &[field, replacement] :
             std::array<std::pair<std::string_view, std::string_view>, 4>{{{"\"maximumContacts\": 32", "\"maximumContacts\": 0"},
                                                                           {"\"steepSlopePolicy\": 1", "\"steepSlopePolicy\": 99"},
                                                                           {"\"enabled\": true", "\"enabled\": \"true\""},
                                                                           {"\"radiusMeters\": 0.5", "\"radiusMeters\": -1.0"}}}) {
            auto encoded = EncodedCharacter(ConfiguredCharacter());
            ReplaceOnce(encoded, field, replacement);
            CHECK(DecodeSceneSource(encoded).HasError());
        }
        auto invalid = ConfiguredCharacter();
        invalid.collision.defaultMaterial.reset();
        CHECK(DecodeSceneSource(EncodedCharacter(invalid)).HasError());
        invalid = ConfiguredCharacter();
        invalid.geometry.up = {0, 2, 0};
        CHECK(Runtime::ValidateCharacterControllerComponent(invalid).HasError());
        invalid = ConfiguredCharacter();
        invalid.geometry.gravity.x = std::numeric_limits<float>::infinity();
        CHECK(Runtime::ValidateCharacterControllerComponent(invalid).HasError());
    }
}  // namespace Horo::SceneSource
