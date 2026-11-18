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

TEST_CASE("Cooked Scene v6 preserves canonical direct child identity and rejects forged evidence",
          "[native][prefab-cook][scene-instances]") {
    const auto asset = Assets::AssetId::Parse("00112233-4455-6677-8899-aabbccddeeff").Value();
    const auto childId = SceneDefinitionIdForAsset(asset);
    const auto source = Runtime::SceneDefinitionBuilder{childId, {12}}.Build();
    REQUIRE(source.HasValue());
    const auto payload = EncodeCookedSceneDefinition(source.Value());
    REQUIRE(payload.HasValue());
    const Runtime::SceneChildDefinitionReference reference{{3},
                                                           asset,
                                                           childId,
                                                           {12},
                                                           ComputeSha256(std::as_bytes(std::span{payload.Value()}))};
    CHECK(DecodeCookedChildScene(payload.Value(), reference).HasValue());
    const auto other = Runtime::SceneDefinitionBuilder{childId, {13}}.Build();
    REQUIRE(other.HasValue());
    const auto stalePayload = EncodeCookedSceneDefinition(other.Value());
    REQUIRE(stalePayload.HasValue());
    CHECK(DecodeCookedChildScene(stalePayload.Value(), reference).HasError());
    Runtime::SceneDefinitionBuilder parent{{1}, {2}};
    REQUIRE(parent.RequireChildScene(reference).HasValue());
    CHECK(parent.RequireChildScene(reference).HasError());
    auto mismatched = reference;
    mismatched.occurrence = {7};
    mismatched.revision = {13};
    CHECK(parent.RequireChildScene(mismatched).HasError());
    const auto definition = std::move(parent).Build();
    REQUIRE(definition.HasValue());
    const auto encoded = EncodeCookedSceneDefinition(definition.Value());
    REQUIRE(encoded.HasValue());
    const auto decoded = DecodeCookedSceneDefinition(encoded.Value(), {1}, {2});
    REQUIRE(decoded.HasValue());
    REQUIRE(decoded.Value().ChildScenes().size() == 1);
    CHECK(decoded.Value().ChildScenes().front() == reference);
    CHECK(EncodeCookedSceneDefinition(decoded.Value()).Value() == encoded.Value());

    const auto reject = [](const nlohmann::json &document) {
        const auto invalid = document.dump() + '\n';
        CHECK(DecodeCookedSceneDefinition(std::span{reinterpret_cast<const std::uint8_t *>(invalid.data()), invalid.size()}, {1}, {2})
                  .HasError());
    };
    const auto wire = nlohmann::json::parse(encoded.Value());
    CHECK(wire["format"].get<std::string>() == ChildSceneCookedSceneFormat);
    auto impossible = wire;
    impossible["childScenes"][0]["scene"] = 4;
    reject(impossible);
    auto missingRevision = wire;
    missingRevision["childScenes"][0].erase("revision");
    reject(missingRevision);
    auto downgrade = wire;
    downgrade["format"] = CookedSceneFormat;
    reject(downgrade);
}

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

TEST_CASE("Scene source and cooked Scene share the exact canvas and its required document asset", "[scene][ui-canvas][cook]") {
    using namespace Horo::SceneSource;
    namespace Ui = Runtime::Ui;
    const auto asset = Assets::AssetId::Parse("00112233-4455-6677-8899-aabbccddeeff").Value();
    const Runtime::UiCanvasComponent component{
        {asset, Ui::UiDocumentId::Create({7}).Value(), Ui::UiCanvasId::Create({4}).Value(), Ui::UiDocumentRevision::Create(2).Value()}};
    const std::array objects{SceneObjectSnapshot{.id = {1},
                                                 .name = "Parent",
                                                 .localTransform = {.translation = {1, 2, 3}},
                                                 .components = {.uiCanvas = component}},
                             SceneObjectSnapshot{.id = {2},
                                                 .parent = SceneObjectId{1},
                                                 .name = "Canvas",
                                                 .localTransform = {.translation = {4, 5, 6}},
                                                 .components = {.uiCanvas = component}}};
    const auto source = DecodeSceneSource(EncodeSceneSource({objects, {}}));
    REQUIRE(source.HasValue());
    const auto scene = ConvertSceneSourceToRuntime({source.Value().objects, {}}, {1}, {2});
    REQUIRE(scene.HasValue());
    const auto dependencies = scene.Value().AssetDependencies();
    REQUIRE(dependencies.size() == 1);
    CHECK(dependencies.front().id == asset);
    CHECK(dependencies.front().expectedType.Value() == Ui::UiDocumentAssetTypeName);
    const auto cooked = EncodeCookedSceneDefinition(scene.Value());
    REQUIRE(cooked.HasValue());
    auto document = nlohmann::json::parse(cooked.Value());
    CHECK(document["format"].get<std::string>() == UiCanvasCookedSceneFormat);
    const auto admitted = DecodeCookedSceneDefinition(cooked.Value(), {1}, {2});
    REQUIRE(admitted.HasValue());
    REQUIRE(admitted.Value().Entities().size() == objects.size());
    for (std::size_t index = 0; index < objects.size(); ++index) {
        CHECK(admitted.Value().Entities()[index].components.uiCanvas == component);
        CHECK(admitted.Value().Entities()[index].localTransform == objects[index].localTransform);
    }
    CHECK(admitted.Value().Entities()[1].parent == Runtime::SceneObjectId{1});

    SECTION("legacy format cannot silently decode a canvas") {
        document["format"] = AnimationCookedSceneFormat;
    }
    SECTION("registry dependency cannot be dropped") {
        document["dependencies"] = nlohmann::json::array();
    }
    SECTION("same asset identity cannot bind an unrelated type") {
        document["dependencies"][0]["type"] = "core.mesh";
    }
    const auto text = document.dump() + '\n';
    const std::vector<std::uint8_t> tampered{text.begin(), text.end()};
    CHECK(DecodeCookedSceneDefinition(tampered, {1}, {2}).HasError());
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

TEST_CASE("Verified cooked Scene admission retains embedded revision without weakening exact revision callers",
          "[scene][character][cook]") {
    const auto encoded = EncodeCookedSceneDefinition(Scene());
    REQUIRE(encoded.HasValue());
    const auto decoded = DecodeCookedSceneWithEmbeddedRevision(encoded.Value(), {1});
    REQUIRE(decoded.HasValue());
    CHECK(decoded.Value().Id() == Runtime::SceneDefinitionId{1});
    CHECK(decoded.Value().Revision() == Runtime::SceneDefinitionRevision{2});
    CHECK(EncodeCookedSceneDefinition(decoded.Value()).Value() == encoded.Value());
    CHECK(DecodeCookedSceneDefinition(encoded.Value(), {1}, {9}).HasError());
    CHECK(DecodeCookedSceneWithEmbeddedRevision(encoded.Value(), {9}).HasError());
}

TEST_CASE("Verified cooked Scene admission rejects missing zero malformed and duplicate durable revisions", "[scene][character][cook]") {
    const auto encoded = EncodeCookedSceneDefinition(Scene()).Value();
    auto document = nlohmann::json::parse(encoded);
    SECTION("zero revision") {
        document["revision"] = std::uint64_t{0};
    }
    SECTION("missing revision") {
        document.erase("revision");
    }
    SECTION("wrong revision type") {
        document["revision"] = "2";
    }
    SECTION("foreign Scene identity") {
        document["scene"] = std::uint64_t{9};
    }
    auto invalid = document.dump() + '\n';
    SECTION("duplicate revision") {
        invalid.insert(1, "\"revision\":2,");
    }
    SECTION("malformed JSON") {
        invalid.front() = '!';
    }
    SECTION("noncanonical bytes") {
        invalid.insert(1, " ");
    }
    const std::vector<std::uint8_t> bytes{invalid.begin(), invalid.end()};
    CHECK(DecodeCookedSceneWithEmbeddedRevision(bytes, {1}).HasError());
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

TEST_CASE("Scene asset identity preserves the canonical SHA256 little-endian compact vector", "[scene][cook][identity]") {
    const auto asset = Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc");
    REQUIRE(asset.HasValue());
    // Independent SHA-256 vector: 38ef79ae6082031faf789a38d68aa7396de05d067a20d5aa3c511d0561e967fd.
    CHECK(SceneDefinitionIdForAsset(asset.Value()) == Runtime::SceneDefinitionId{2234773191861595960ULL});
}
