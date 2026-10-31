#include "CharacterPropertyCorpus.h"
#include "Horo/Scene/SceneSource.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <nlohmann/json.hpp>
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

    TEST_CASE("Character source rejects malformed nested wire values with actionable errors", "[scene][character][serialization]") {
        using Json = nlohmann::json;
        auto character = ConfiguredCharacter();
        character.collision.excludedBody = Runtime::PhysicsBodyReference{{17}, {9}};
        const Json document = Json::parse(EncodedCharacter(character));
        const auto valid = DecodeSceneSource(document.dump());
        REQUIRE(valid.HasValue());
        CHECK(valid.Value().objects[0].components.characterController == character);
        const std::array<std::pair<std::string_view, Json>, 20> invalidValues{
            {{"/id", 0},
             {"/schemaVersion", 7},
             {"/enabled", "true"},
             {"/geometry", false},
             {"/geometry/capsule", false},
             {"/geometry/capsule/radiusMeters", "wide"},
             {"/geometry/up", Json::array({0, 0})},
             {"/geometry/crouchedCapsule", false},
             {"/movement", false},
             {"/movement/skinWidthMeters", "wide"},
             {"/movement/dynamicPlatformSupport", 1},
             {"/movement/platformLeaveVelocity", 3},
             {"/movement/maximumContacts", std::numeric_limits<std::uint64_t>::max()},
             {"/collision", false},
             {"/collision/profile", 1},
             {"/collision/queryChannel", "invalid"},
             {"/collision/defaultMaterial/asset", "invalid"},
             {"/collision/defaultMaterial/slot", -1},
             {"/collision/excludedBody/object", "seventeen"},
             {"/collision/excludedBody/body", 0}}};
        for (const auto &[path, value] : invalidValues) {
            CAPTURE(path);
            Json invalid = document;
            invalid["objects"][0]["components"]["characterController"].at(Json::json_pointer{std::string{path}}) = value;
            const auto result = DecodeSceneSource(invalid.dump());
            REQUIRE(result.HasError());
            CHECK_FALSE(result.ErrorValue().message.empty());
        }
    }

    TEST_CASE("Character source requires complete records and rejects duplicate component keys", "[scene][character][serialization]") {
        using Json = nlohmann::json;
        const auto encoded = EncodedCharacter(ConfiguredCharacter());
        const Json document = Json::parse(encoded);
        for (const std::string_view path :
             {"/id", "/schemaVersion", "/enabled", "/geometry", "/movement", "/collision", "/geometry/capsule",
              "/geometry/capsule/radiusMeters", "/geometry/up", "/movement/maximumContacts", "/movement/skinWidthMeters",
              "/collision/profile", "/collision/queryChannel", "/collision/defaultMaterial/asset"}) {
            CAPTURE(path);
            Json invalid = document;
            const Json::json_pointer pointer{std::string{path}};
            auto &component = invalid["objects"][0]["components"]["characterController"];
            REQUIRE(component.at(pointer.parent_pointer()).erase(pointer.back()) == 1);
            const auto result = DecodeSceneSource(invalid.dump());
            REQUIRE(result.HasError());
            CHECK_FALSE(result.ErrorValue().message.empty());
        }
        auto duplicate = encoded;
        ReplaceOnce(duplicate, "\"id\": 7", "\"id\": 7, \"id\": 7");
        const auto result = DecodeSceneSource(duplicate);
        REQUIRE(result.HasError());
        CHECK_FALSE(result.ErrorValue().message.empty());
    }

    TEST_CASE("Generated Character wire records roundtrip and reject malformed schema payloads",
              "[scene][character][character-property][serialization][fuzz]") {
        Character::PropertyTest::Corpus("character-wire", [](const Character::PropertyTest::Recipe &input) -> std::string {
            auto character = ConfiguredCharacter();
            character.id = {1ULL + input.words[2]};
            character.geometry.capsule.radiusMeters = 0.25F + static_cast<float>(input.words[3] % 16) / 16;
            character.geometry.crouchedCapsule->radiusMeters = character.geometry.capsule.radiusMeters;
            character.movement.maximumContacts = 1 + input.words[4] % Character::MaximumCharacterContacts;
            const auto encoded = EncodedCharacter(character);
            const auto decoded = DecodeSceneSource(encoded);
            if (decoded.HasError() || decoded.Value().objects.size() != 1 ||
                decoded.Value().objects[0].components.characterController != character)
                return "generated wire roundtrip changed durable character";
            if (EncodeSceneSource({decoded.Value().objects, decoded.Value().prefabInstances}) != encoded)
                return "canonical character encoding changed on replay";
            auto document = nlohmann::json::parse(encoded);
            auto &wire = document["objects"][0]["components"]["characterController"];
            switch (input.words[0] % 8) {
                case 0:
                    wire["schemaVersion"] = 0;
                    break;
                case 1:
                    wire["schemaVersion"] = Runtime::CharacterSceneComponentSchemaVersion + 1ULL + input.words[5];
                    break;
                case 2:
                    document["schemaVersion"] = 1;
                    break;
                case 3:
                    wire["geometry"]["capsule"]["radiusMeters"] = "NaN";
                    break;
                case 4:
                    wire["geometry"]["up"] = nlohmann::json::array({0, 0});
                    break;
                case 5:
                    wire["movement"]["maximumContacts"] = std::numeric_limits<std::uint64_t>::max();
                    break;
                case 6:
                    wire.erase("geometry");
                    break;
                default:
                    wire["collision"]["defaultMaterial"]["asset"] = "malformed";
                    break;
            }
            const auto malformed = document.dump();
            const auto rejected = DecodeSceneSource(malformed);
            if (rejected.HasValue() || rejected.ErrorValue().code.Value().empty() || rejected.ErrorValue().message.empty())
                return "malformed character wire accepted or untyped";
            const auto truncated = DecodeSceneSource(encoded.substr(0, input.words[6] % encoded.size()));
            if (truncated.HasValue())
                return "truncated character wire accepted";
            return {};
        });
    }
}  // namespace Horo::SceneSource
