#include "Horo/Scene/SceneSource.h"

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

    TEST_CASE("Scene source rejects malformed future and structurally incomplete input", "[unit][scene][serialization]") {
        CHECK(DecodeSceneSource("{").HasError());
        CHECK(DecodeSceneSource(R"({"schemaVersion":2,"objects":[]})").HasError());
        CHECK(DecodeSceneSource(R"({"schemaVersion":1,"objects":[{"id":1}]})").HasError());
        CHECK(DecodeSceneSource(R"({"schemaVersion":1,"objects":[],"prefabInstances":{}})").HasError());
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
}  // namespace Horo::SceneSource
