#include "Horo/Scene/CookedSceneDefinition.h"
#include "Horo/Scene/SceneSource.h"
#include "TriggerVolumeConversionTestValues.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::SceneCook;

    /** @brief Creates typed content whose disabled values must survive runtime wire encoding. */
    Runtime::RuntimeSceneDefinition Scene() {
        Runtime::SceneDefinitionBuilder builder{{1}, {2}};
        builder.Add({.object = {900}, .components = {.camera = Runtime::CameraComponent{.enabled = false}}});
        builder.Add({.object = {901}, .parent = Runtime::SceneObjectId{900}, .localTransform = {.translation = {0, 4, 0}}});
        auto scene = std::move(builder).Build();
        REQUIRE(scene.HasValue());
        return std::move(scene).Value();
    }
}  // namespace

TEST_CASE("Cooked scene payload is canonical source-free and round trips typed runtime content", "[native][prefab-cook]") {
    const auto scene = Scene();
    const auto encoded = EncodeCookedSceneDefinition(scene);
    REQUIRE(encoded.HasValue());
    const std::string text{encoded.Value().begin(), encoded.Value().end()};
    REQUIRE(text.find("prefabInstances") == std::string::npos);
    REQUIRE(text.find("sourceAsset") == std::string::npos);
    REQUIRE(text.find("editor") == std::string::npos);
    REQUIRE(text.find("name") == std::string::npos);
    const auto decoded = DecodeCookedSceneDefinition(encoded.Value(), {1}, {2});
    REQUIRE(decoded.HasValue());
    REQUIRE(decoded.Value().Entities().size() == 2);
    REQUIRE(decoded.Value().Entities()[0].components == scene.Entities()[0].components);
    REQUIRE(decoded.Value().Entities()[1].parent == scene.Entities()[1].parent);
    REQUIRE(decoded.Value().Entities()[1].localTransform == scene.Entities()[1].localTransform);
    const auto again = EncodeCookedSceneDefinition(decoded.Value());
    REQUIRE(again.HasValue());
    REQUIRE(again.Value() == encoded.Value());
}

TEST_CASE("Cooked scene payload rejects foreign malformed and noncanonical input", "[native][prefab-cook]") {
    const auto encoded = EncodeCookedSceneDefinition(Scene());
    REQUIRE(encoded.HasValue());
    REQUIRE(DecodeCookedSceneDefinition(encoded.Value(), {9}, {2}).HasError());
    REQUIRE(DecodeCookedSceneDefinition(encoded.Value(), {1}, {9}).HasError());
    auto bytes = encoded.Value();
    bytes.pop_back();
    REQUIRE(DecodeCookedSceneDefinition(bytes, {1}, {2}).HasError());
    bytes = encoded.Value();
    bytes.front() = '!';
    REQUIRE(DecodeCookedSceneDefinition(bytes, {1}, {2}).HasError());
    bytes = encoded.Value();
    const std::string raw = "\"prefabInstances\":[],";
    bytes.insert(bytes.begin() + 1, raw.begin(), raw.end());
    REQUIRE(DecodeCookedSceneDefinition(bytes, {1}, {2}).HasError());
}

TEST_CASE("Cooked scene payload honors the same exact bounds for encoding and decoding", "[native][prefab-cook]") {
    const auto scene = Scene();
    const auto encoded = EncodeCookedSceneDefinition(scene);
    REQUIRE(encoded.HasValue());
    CookedSceneLimits limits{.maximumEntities = 2, .maximumDependencies = 1, .maximumBytes = encoded.Value().size()};
    REQUIRE(EncodeCookedSceneDefinition(scene, limits).HasValue());
    REQUIRE(DecodeCookedSceneDefinition(encoded.Value(), {1}, {2}, limits).HasValue());
    SECTION("entity count") {
        limits.maximumEntities = 1;
    }
    SECTION("payload bytes") {
        --limits.maximumBytes;
    }
    REQUIRE(EncodeCookedSceneDefinition(scene, limits).HasError());
    REQUIRE(DecodeCookedSceneDefinition(encoded.Value(), {1}, {2}, limits).HasError());
}

TEST_CASE("Trigger migration proof survives cooked bytes and rejects malformed required metadata", "[native][trigger][prefab-cook]") {
    SceneSource::SceneObjectSnapshot object{.id = {874}, .components = {.triggerVolume = Runtime::TriggerVolumeComponent{}}};
    const std::array objects{object};
    const auto converted = SceneSource::ConvertSceneSourceToRuntime({objects, {}}, {1}, {2}, Test::TriggerBinding());
    REQUIRE(converted.HasValue());
    REQUIRE(converted.Value().AssetDependencies().size() == 1);
    const auto encoded = EncodeCookedSceneDefinition(converted.Value());
    REQUIRE(encoded.HasValue());
    const auto roundtrip = DecodeCookedSceneDefinition(encoded.Value(), {1}, {2});
    REQUIRE(roundtrip.HasValue());
    REQUIRE(roundtrip.Value().TriggerVolumeEvidence().front() == converted.Value().TriggerVolumeEvidence().front());
    const std::string bytes{encoded.Value().begin(), encoded.Value().end()};
    auto document = nlohmann::json::parse(bytes);
    SECTION("old cooked versions require recook") {
        document["format"] = "horo.scene.runtime.v1";
    }
    SECTION("required evidence field cannot disappear") {
        document.erase("triggerMigration");
    }
    SECTION("duplicate object proof rejects") {
        document["triggerMigration"].push_back(document["triggerMigration"][0]);
    }
    SECTION("zero project fingerprint rejects") {
        document["triggerMigration"][0]["schema"] = std::uint64_t{0};
    }
    SECTION("malformed material digest rejects") {
        document["triggerMigration"][0]["payload"] = "not-a-digest";
    }
    SECTION("foreign material identity rejects") {
        document["triggerMigration"][0]["material"] = "00000000-0000-0000-0000-000000000099";
    }
    SECTION("component disagreement rejects") {
        document["triggerMigration"][0]["collider"] = std::uint64_t{99};
    }
    const std::string invalid = document.dump() + '\n';
    REQUIRE(DecodeCookedSceneDefinition(std::span{reinterpret_cast<const std::uint8_t *>(invalid.data()), invalid.size()}, {1}, {2})
                .HasError());
}

TEST_CASE("Trigger binding union rejects conflicting semantic authorities and duplicate producers", "[native][trigger][scene]") {
    const auto context = Test::TriggerBinding();
    Runtime::SceneDefinitionBuilder builder{{1}, {1}};
    Runtime::TriggerVolumeMigrationEvidence first{{1}, {1}, {1}, context.binding};
    REQUIRE(builder.RequireTriggerVolume(first).HasValue());
    SECTION("duplicate producers cannot win by insertion order") {
        REQUIRE(builder.RequireTriggerVolume(first).HasError());
    }
    SECTION("different project schemas cannot be combined") {
        auto other = first;
        other.object = {2};
        ++other.binding.collisionSchemaFingerprint;
        REQUIRE(builder.RequireTriggerVolume(other).HasError());
    }
    SECTION("same material cannot denote two payloads") {
        auto other = first;
        other.object = {2};
        other.binding.materialPayloadDigest.bytes.front() ^= 1;
        REQUIRE(builder.RequireTriggerVolume(other).HasError());
    }
}

TEST_CASE("Cooked scene decoding rejects unhandled components without dropping recognized portable values", "[native][prefab-cook]") {
    const auto encoded = EncodeCookedSceneDefinition(Scene());
    REQUIRE(encoded.HasValue());
    auto document = nlohmann::json::parse(encoded.Value());
    SECTION("an opaque future component cannot be erased from runtime input") {
        document["entities"][0]["components"]["futureCollisionOwner"] = {{"enabled", false}};
    }
    SECTION("a null unknown key is still unhandled schema evidence") {
        document["entities"][0]["components"]["futureCollisionOwner"] = nullptr;
    }
    SECTION("recognized authoring-only trigger values remain unsupported in cooked input") {
        const SceneSource::SceneObjectSnapshot source{.id = {1}, .components = {.triggerVolume = Runtime::TriggerVolumeComponent{}}};
        const std::array objects{source};
        const auto authored = nlohmann::json::parse(SceneSource::EncodeSceneSource({objects, {}}));
        document["entities"][0]["components"]["triggerVolume"] = authored["objects"][0]["components"]["triggerVolume"];
    }
    const auto text = document.dump() + '\n';
    const auto decoded = DecodeCookedSceneDefinition(std::span{reinterpret_cast<const std::uint8_t *>(text.data()), text.size()}, {1}, {2});
    REQUIRE(decoded.HasError());
    REQUIRE(decoded.ErrorValue().domain.Value() == SceneCookErrors::Unsupported.domain.Value());
    REQUIRE(decoded.ErrorValue().code.Value() == SceneCookErrors::Unsupported.code.Value());
    const auto valid = DecodeCookedSceneDefinition(encoded.Value(), {1}, {2});
    REQUIRE(valid.HasValue());
    REQUIRE(valid.Value().Entities().front().components.camera.has_value());
    REQUIRE_FALSE(valid.Value().Entities().front().components.camera->enabled);
}
