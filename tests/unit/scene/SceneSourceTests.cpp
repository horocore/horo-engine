#include "Horo/Scene/SceneSource.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>

namespace Horo::SceneSource {
    TEST_CASE("Scene source retains the schema-1 canonical empty-document bytes", "[unit][scene][serialization]") {
        const std::string expected = "{\n  \"objects\": [],\n  \"prefabInstances\": [],\n  \"schemaVersion\": 1\n}\n";
        const auto decoded = DecodeSceneSource(expected);
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().objects.empty());
        CHECK(decoded.Value().prefabInstances.empty());
        CHECK(EncodeSceneSource({decoded.Value().objects, decoded.Value().prefabInstances}) == expected);
    }

    TEST_CASE("Scene source round trip preserves identity typed components and inert editor state", "[unit][scene][serialization]") {
        SceneSourceDocument source;
        source.objects.push_back(SceneObjectSnapshot{
            .id = {42},
            .name = "quoted \"scene\"",
            .components = {.camera = Runtime::CameraComponent{}},
            .editorState = {.visible = false, .locked = true},
        });
        const std::string encoded = EncodeSceneSource({source.objects, source.prefabInstances});
        const auto decoded = DecodeSceneSource(encoded);
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().objects.size() == 1);
        const auto &object = decoded.Value().objects.front();
        CHECK(object.id == source.objects.front().id);
        CHECK(object.name == source.objects.front().name);
        CHECK(object.components == source.objects.front().components);
        CHECK(object.editorState == source.objects.front().editorState);
        CHECK(EncodeSceneSource({decoded.Value().objects, decoded.Value().prefabInstances}) == encoded);
    }

    TEST_CASE("Scene source round trips every distinct voice admission policy", "[unit][scene][serialization][audio]") {
        using enum Audio::AudioConcurrencyMode;
        for (const auto mode : {Allow, Reject, StealOldest, StealQuietest, Virtualize, StealLowestPriority, StealFurthest, Replace}) {
            SceneSourceDocument source;
            Runtime::AudioSourceComponent audio;
            audio.playback.concurrency.mode = mode;
            source.objects.push_back(SceneObjectSnapshot{.id = {1}, .name = "Audio", .components = {.audioSource = audio}});
            const auto encoded = EncodeSceneSource({source.objects, source.prefabInstances});
            const auto decoded = DecodeSceneSource(encoded);
            REQUIRE(decoded.HasValue());
            REQUIRE(decoded.Value().objects.size() == 1);
            REQUIRE(decoded.Value().objects.front().components.audioSource.has_value());
            CHECK(decoded.Value().objects.front().components.audioSource->playback.concurrency.mode == mode);
            CHECK(EncodeSceneSource({decoded.Value().objects, decoded.Value().prefabInstances}) == encoded);
        }
    }

    TEST_CASE("Scene source rejects malformed listener values without inventing legacy listeners", "[unit][scene][serialization][audio]") {
        const std::string prefix =
            R"({"schemaVersion":1,"objects":[{"id":1,"parent":null,"name":"Listener","transform":{"translation":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},"primitiveMesh":null,"components":)";
        const std::string suffix = "}]}";
        for (const auto listener :
             {R"({"view":17,"priority":0,"weight":1,"enabled":true})", R"({"view":-1,"priority":0,"weight":1,"enabled":true})",
              R"({"view":0,"priority":2147483648,"weight":1,"enabled":true})",
              R"({"view":0,"priority":18446744073709551615,"weight":1,"enabled":true})",
              R"({"view":0,"priority":-2147483649,"weight":1,"enabled":true})", R"({"view":0,"priority":0,"weight":0,"enabled":true})",
              R"({"view":0,"priority":0,"weight":-1,"enabled":true})", R"({"view":0,"priority":0,"weight":1,"enabled":"true"})",
              R"({"view":0,"priority":0,"weight":1})"}) {
            INFO(listener);
            CHECK(DecodeSceneSource(prefix + "{\"audioListener\":" + listener + "}" + suffix).HasError());
        }
        const auto legacy = DecodeSceneSource(prefix + "{}" + suffix);
        REQUIRE(legacy.HasValue());
        CHECK_FALSE(legacy.Value().objects.front().components.audioListener);
    }

    TEST_CASE("Scene source rejects malformed future and structurally incomplete input", "[unit][scene][serialization]") {
        CHECK(DecodeSceneSource("{").HasError());
        CHECK(DecodeSceneSource(R"({"schemaVersion":6,"objects":[]})").HasError());
        CHECK(DecodeSceneSource(R"({"schemaVersion":1,"objects":[{"id":1}]})").HasError());
        CHECK(DecodeSceneSource(R"({"schemaVersion":1,"objects":[],"prefabInstances":{}})").HasError());
    }

    TEST_CASE("Scene source admits exact canvas references in schema five", "[unit][scene][serialization][ui-canvas]") {
        namespace Ui = Runtime::Ui;
        const auto asset = Assets::AssetId::Parse("00112233-4455-6677-8899-aabbccddeeff").Value();
        const auto component = Runtime::UiCanvasComponent{
            {asset, Ui::UiDocumentId::Create({4}).Value(), Ui::UiCanvasId::Create({8}).Value(), Ui::UiDocumentRevision::Create(3).Value()}};
        const std::array objects{SceneObjectSnapshot{.id = {1}, .name = "Canvas", .components = {.uiCanvas = component}}};
        const auto encoded = EncodeSceneSource({objects, {}});
        CHECK(encoded.find("\"schemaVersion\": 5") != std::string::npos);
        const auto decoded = DecodeSceneSource(encoded);
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().objects.size() == 1);
        CHECK(decoded.Value().objects.front().components.uiCanvas == component);
        CHECK(EncodeSceneSource({decoded.Value().objects, {}}) == encoded);
        auto malformed = encoded;
        std::string_view field;
        std::string_view replacement;
        SECTION("downgraded version") {
            field = "\"schemaVersion\": 5";
            replacement = "\"schemaVersion\": 4";
        }
        SECTION("missing identity") {
            field = "\"minimumRevision\": 3";
            replacement = "\"unused\": 3";
        }
        SECTION("zero canvas") {
            field = "\"canvas\": \"08000000000000000000000000000000\"";
            replacement = "\"canvas\": \"00000000000000000000000000000000\"";
        }
        const auto position = malformed.find(field);
        REQUIRE(position != std::string::npos);
        malformed.replace(position, field.size(), replacement);
        CHECK(DecodeSceneSource(malformed).HasError());
    }

    TEST_CASE("Scene source retains legacy absent prefab instances without inventing objects", "[unit][scene][serialization]") {
        const auto decoded = DecodeSceneSource(R"({"schemaVersion":1,"objects":[]})");
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().objects.empty());
        CHECK(decoded.Value().prefabInstances.empty());
    }

    TEST_CASE("Scene source bounds detached input before JSON parsing", "[unit][scene][serialization]") {
        CHECK(DecodeSceneSource("").HasError());
        const std::string oversized(16U * 1024U * 1024U + 1U, ' ');
        CHECK(DecodeSceneSource(oversized).HasError());
    }

    TEST_CASE("Scene source bounds ignored nested fields and rejects duplicate keys", "[unit][scene][serialization]") {
        const auto nested = [](const std::size_t arrays) {
            return std::string{R"({"schemaVersion":1,"objects":[],"unknown":)"} + std::string(arrays, '[') + "0" +
                   std::string(arrays, ']') + "}";
        };
        CHECK(DecodeSceneSource(nested(62)).HasValue());
        CHECK(DecodeSceneSource(nested(63)).HasError());
        CHECK(DecodeSceneSource(nested(4096)).HasError());
        CHECK(DecodeSceneSource(R"({"schemaVersion":1,"schemaVersion":1,"objects":[]})").HasError());
        CHECK(DecodeSceneSource(R"({"schemaVersion":1,"objects":[],"unknown":{"key":1,"key":2}})").HasError());
        CHECK(DecodeSceneSource(R"({"schemaVersion":1,"objects":[],"unknown":[{"broken":})").HasError());
    }
}  // namespace Horo::SceneSource
